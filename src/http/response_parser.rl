/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#ifndef SEASTAR_MODULE
#include <memory>
#include <unordered_map>
#endif

#include <seastar/core/ragel.hh>
#include <seastar/util/modules.hh>
#include <seastar/http/reply.hh>

namespace seastar {

SEASTAR_MODULE_EXPORT_BEGIN

%% machine reply;

%%{

access _fsm_;

action mark {
    g.mark_start(p);
}

action store_version {
    _rsp->_version = str();
}

action store_field_name {
    _field_name = str();
}

action store_value {
    _value = str();
}

action trim_trailing_whitespace_and_store_value {
    _value = str();
    trim_trailing_spaces_and_tabs(_value);
    g.mark_start(nullptr);
}

action assign_field {
    auto [iter, inserted] = _rsp->_headers.try_emplace(_field_name, std::move(_value));
    if (!inserted) {
        // RFC 7230, section 3.2.2.  Field Parsing:
        // A recipient MAY combine multiple header fields with the same field name into one
        // "field-name: field-value" pair, without changing the semantics of the message,
        // by appending each subsequent field value to the combined field value in order, separated by a comma.
        iter->second += sstring(",") + std::move(_value);
    }
}

action extend_field  {
    // RFC 7230, section 3.2.4.  Field Order:
    // A server that receives an obs-fold in a request message that is not
    // within a message/http container MUST either reject the message [...]
    // or replace each received obs-fold with one or more SP octets [...]
    _rsp->_headers[_field_name] += sstring(" ") + std::move(_value);
}

action store_status {
    _rsp->_status = static_cast<http::reply::status_type>(std::atoi(str().c_str()));
}

action done {
    done = true;
    fbreak;
}

cr = '\r';
lf = '\n';
crlf = '\r\n';
tchar = alpha | digit | '-' | '!' | '#' | '$' | '%' | '&' | '\'' | '*'
        | '+' | '.' | '^' | '_' | '`' | '|' | '~';

sp = ' ';
ht = '\t';

sp_ht = sp | ht;

http_version = 'HTTP/' (digit '.' digit) >mark %store_version;

obs_text = 0x80..0xFF; # defined in RFC 7230, Section 3.2.6.
field_vchar = (graph | obs_text);
# RFC 9110, Section 5.5 allows single ' '/'\t' separators between
# field_vchar words. We are less strict and allow any number of spaces
# between words.
# Trailing spaces are trimmed in postprocessing.
field_content = (field_vchar | sp_ht)*;

field = tchar+ >mark %store_field_name;
value = field_content >mark %trim_trailing_whitespace_and_store_value;
status_code = (digit digit digit) >mark %store_status;
start_line = http_version space status_code space (any - cr - lf)* crlf;
header_1st = (field ':' sp_ht* <: value crlf) %assign_field;
header_cont = (sp_ht+ <: value crlf) %extend_field;
header = header_1st header_cont*;
main := start_line header* (crlf @done);

}%%

class http_response_parser : public ragel_parser_base<http_response_parser> {
    %% write data nofinal noprefix;
public:
    enum class state {
        error,
        eof,
        done,
    };
    std::unique_ptr<http::reply> _rsp;
    sstring _field_name;
    sstring _value;
    state _state;
    sstring _error_message;

public:
    void init() {
        init_base();
        _rsp.reset(new http::reply());
        _state = state::eof;
        _error_message = {};
        %% write init;
    }
    char* parse(char* p, char* pe, char* eof) {
        // Save the start pointer to compute offsets for error context.
        char* start_ptr = p;
        sstring_builder::guard g(_builder, p, pe);
        auto str = [this, &g, &p] { g.mark_end(p); return get_str(); };
        bool done = false;
        if (p != pe) {
            _state = state::error;
        }
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmisleading-indentation"
#endif
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
        %% write exec;
#pragma GCC diagnostic pop
#ifdef __clang__
#pragma clang diagnostic pop
#endif
        if (done) {
            _state = state::done;
        } else if (p == eof) {
            _state = state::eof;
            _error_message = "Incomplete HTTP response header: reached end-of-file before parsing completed.";
        } else if (p != pe) {
            _state = state::error;
            // Get the error offset and extract a snippet from the current pointer.
            size_t offset = static_cast<size_t>(p - start_ptr);
            size_t available = static_cast<size_t>(pe - p);
            size_t context_len = std::min(available, 32ul);
            sstring encountered(p, context_len);
            _error_message = sstring("Parsing error at offset ") + std::to_string(offset) + ": encountered \"" + encountered +
                                         "\". Expected valid HTTP response header format (e.g., a complete start line with HTTP version, three-digit status code, header "
                                         "fields, and a terminating CRLF).";
        } else {
            p = nullptr;
        }
        return p;
    }
    auto get_parsed_response() {
        return std::move(_rsp);
    }
    bool eof() const {
        return _state == state::eof;
    }
    bool failed() const {
        return _state == state::error;
    }
    const sstring& error_message() const {
        return _error_message;
    }
};
SEASTAR_MODULE_EXPORT_END
}
