/*
 * HTTP/2 Frame Header Parser
 * Parses the 9-byte HTTP/2 frame header into a struct.
 */

#pragma once

#include <seastar/core/ragel.hh>
#include <cstdint>

namespace seastar {

%%{
    machine http2_frame_parser;

    access _fsm_;

    action mark_start {
        _frame_start = p;
    }

    action store_length1 { 
        _length = (uint32_t)(uint8_t)_frame_start[0] << 16; 
    }
    action store_length2 {
        _length |= (uint32_t)(uint8_t)_frame_start[1] << 8; 
    }
    action store_length3 { 
        _length |= (uint32_t)(uint8_t)_frame_start[2]; 
    }
    action store_type { 
        _type = (uint8_t)_frame_start[3]; 
    }
    action store_flags { 
        _flags = (uint8_t)_frame_start[4]; 
    }

    main := (
        any >mark_start
        any @store_length1
        any @store_length2
        any @store_length3
        any @store_type
        any @store_flags
    );
}%%

class http2_frame_header_parser : public ragel_parser_base<http2_frame_header_parser> {
    %% write data nofinal noprefix;
private:
    const char* _frame_start = nullptr;

public:
    uint32_t _length = 0;
    uint8_t _type = 0;
    uint8_t _flags = 0;

    void init() {
        init_base();
        _length = 0;
        _type = 0;
        _flags = 0;
        %% write init;
    }

    // Returns pointer to next byte after header if parsed, nullptr otherwise
    char* parse(char* p, char* pe) {
        char* start = p;
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
        if (p - start >= 9) {
            return p;
        }
        return nullptr;
    }
};

} // namespace seastar 