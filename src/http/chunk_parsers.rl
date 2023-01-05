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
 * Copyright (C) 2020 Cloudius Systems, Ltd.
 */

#pragma once

#include <seastar/core/ragel.hh>
#include <seastar/core/sstring.hh>
#include <unordered_map>

namespace seastar {

%% machine chunk_head;

%%{

access _fsm_;

action mark {
    g.mark_start(p);
}

action store_name {
    _name = str();
}

action store_value {
    _value = str();
}

action store_size {
    _size = str();
}

action assign_value {
    _extensions[_name] = std::move(_value);
}

action done {
    done = true;
    fbreak;
}

crlf  = '\r\n';
sp    = ' ';
ht    = '\t';
sp_ht = sp | ht;
tchar = alpha | digit | '-' | '!' | '#' | '$' | '%' | '&' | '\'' | '*'
        | '+' | '.' | '^' | '_' | '`' | '|' | '~';

# names correspond to the ones in RFC 7230 (with hyphens instead of underscores)
obs_text = 0x80..0xFF;
qdtext = any - (cntrl | '"' | '\\');
quoted_pair = ('\\' >{ g.mark_end(p); }) ((any - cntrl) >mark) ;
token = tchar+;
quoted_string = '"' ((qdtext | quoted_pair)* >mark %store_value) '"';
chunk_ext_name = token >mark %store_name;
chunk_ext_val = (token >mark %store_value) | quoted_string ;
chunk_size = xdigit+ >mark %store_size;
chunk_ext = (';' chunk_ext_name ('=' chunk_ext_val)? %assign_value)*;

main := chunk_size chunk_ext crlf @done;

}%%

class http_chunk_size_and_ext_parser : public ragel_parser_base<http_chunk_size_and_ext_parser> {
    %% write data nofinal noprefix;
public:
    enum class state {
        error,
        eof,
        done,
    };
    std::unordered_map<sstring, sstring> _extensions;
    sstring _name;
    sstring _value;
    sstring _size;
    state _state;
public:
    void init() {
        init_base();
        _extensions = std::unordered_map<sstring, sstring>();
        _value = "";
        _size = "";
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
    auto get_parsed_extensions() {
        return std::move(_extensions);
    }
    auto get_size() {
        return std::move(_size);
    }
    bool eof() const {
        return _state == state::eof;
    }
    bool failed() const {
        return _state == state::error;
    }
};

%% machine chunk_tail;
// the headers in the chunk trailer are parsed the same way as in the request_parser.rl
%%{

access _fsm_;

action mark {
    g.mark_start(p);
}

action store_field_name {
    _field_name = str();
}

action trim_trailing_whitespace_and_store_value {
    _value = str();
    trim_trailing_spaces_and_tabs(_value);
    g.mark_start(nullptr);
}

action assign_field {
    if (_headers.count(_field_name)) {
        _headers[_field_name] += sstring(",") + std::move(_value);
    } else {
        _headers[_field_name] = std::move(_value);
    }
}

action extend_field  {
    _headers[_field_name] += sstring(" ") + std::move(_value);
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

obs_text = 0x80..0xFF; # defined in RFC 7230, Section 3.2.6.
field_vchar = (graph | obs_text);
# RFC 9110, Section 5.5 allows single ' '/'\t' separators between
# field_vchar words. We are less strict and allow any number of spaces
# between words.
# Trailing spaces are trimmed in postprocessing.
field_content = (field_vchar | sp_ht)*;

field = tchar+ >mark %store_field_name;
value = field_content >mark %trim_trailing_whitespace_and_store_value;
header_1st = field ':' sp_ht* <: value crlf %assign_field;
header_cont = (sp_ht+ <: value crlf) %extend_field;
header = header_1st header_cont*;
main := header* crlf @done;

}%%

class http_chunk_trailer_parser : public ragel_parser_base<http_chunk_trailer_parser> {
    %% write data nofinal noprefix;
public:
    enum class state {
        error,
        eof,
        done,
    };
    std::unordered_map<sstring, sstring> _headers;
    sstring _field_name;
    sstring _value;
    state _state;
public:
    void init() {
        init_base();
        _headers = std::unordered_map<sstring, sstring>();
        _field_name = "";
        _value = "";
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
    auto get_parsed_headers() {
        return std::move(_headers);
    }
    bool eof() const {
        return _state == state::eof;
    }
    bool failed() const {
        return _state == state::error;
    }
};

}
