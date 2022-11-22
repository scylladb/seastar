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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#pragma once

#include <seastar/core/ragel.hh>
#include <memory>
#include <unordered_map>
#include <seastar/http/request.hh>

namespace seastar {

using namespace httpd;

%% machine request;

%%{

access _fsm_;

action mark {
    g.mark_start(p);
}

action store_method {
    _req->_method = str();
}

action store_uri {
    _req->_url = str();
}

action store_version {
    _req->_version = str();
}

action store_field_name {
    _field_name = str();
}

action store_value {
    _value = str();
}

action no_mark_store_value {
    _value = get_str();
    g.mark_start(nullptr);
}

action checkpoint {
    // Needs a start to be marked beforehand.
    // Used to mark the candidate end of value string. Can be moved furhter by repetitive use
    // of this action.
    // To store the string that ends on the last checkpoint (instead of the last processed character)
    // use %no_mark_store_value instead of %store_value
    g.mark_end(p);
    g.mark_start(p);
}

action assign_field {
    if (_req->_headers.count(_field_name)) {
        // RFC 7230, section 3.2.2.  Field Parsing:
        // A recipient MAY combine multiple header fields with the same field name into one
        // "field-name: field-value" pair, without changing the semantics of the message,
        // by appending each subsequent field value to the combined field value in order, separated by a comma.
        _req->_headers[_field_name] += sstring(",") + std::move(_value);
    } else {
        _req->_headers[_field_name] = std::move(_value);
    }
}

action extend_field  {
    // RFC 7230, section 3.2.4.  Field Order:
    // A server that receives an obs-fold in a request message that is not
    // within a message/http container MUST either reject the message [...]
    // or replace each received obs-fold with one or more SP octets [...]
    _req->_headers[_field_name] += sstring(" ") + std::move(_value);
}

action done {
    done = true;
    fbreak;
}

crlf = '\r\n';
tchar = alpha | digit | '-' | '!' | '#' | '$' | '%' | '&' | '\'' | '*'
        | '+' | '.' | '^' | '_' | '`' | '|' | '~';

sp = ' ';
ht = '\t';

sp_ht = sp | ht;

op_char = upper;

operation = op_char+ >mark %store_method;
uri = (any - sp)+ >mark %store_uri;
http_version = 'HTTP/' (digit '.' digit) >mark %store_version;

obs_text = 0x80..0xFF; # defined in RFC 7230, Section 3.2.6.
field_vchars = (graph | obs_text)+ %checkpoint;
field_content = (field_vchars sp_ht*)*;

field = tchar+ >mark %store_field_name;
value = field_content >mark %no_mark_store_value;
start_line = ((operation sp uri sp http_version) -- crlf) crlf;
header_1st = (field ':' sp_ht* value crlf) %assign_field;
header_cont = (sp_ht+ value crlf) %extend_field;
header = header_1st header_cont*;
main := start_line header* (crlf @done);

}%%

class http_request_parser : public ragel_parser_base<http_request_parser> {
    %% write data nofinal noprefix;
public:
    enum class state {
        error,
        eof,
        done,
    };
    std::unique_ptr<http::request> _req;
    sstring _field_name;
    sstring _value;
    state _state;
public:
    void init() {
        init_base();
        _req.reset(new http::request());
        _state = state::eof;
        %% write init;
    }
    char* parse(char* p, char* pe, char* eof) {
        sstring_builder::guard g(_builder, p, pe);
        [[maybe_unused]] auto str = [this, &g, &p] { g.mark_end(p); return get_str(); };
        bool done = false;
        if (p != pe) {
            _state = state::error;
        }
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmisleading-indentation"
#endif
        %% write exec;
#ifdef __clang__
#pragma clang diagnostic pop
#endif
        if (!done) {
            if (p == eof) {
                _state = state::eof;
            } else if (p != pe) {
                _state = state::error;
            } else {
                p = nullptr;
            }
        } else {
            _state = state::done;
        }
        return p;
    }
    auto get_parsed_request() {
        return std::move(_req);
    }
    bool eof() const {
        return _state == state::eof;
    }
    bool failed() const {
        return _state == state::error;
    }
};

}
