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
 * Copyright (C) 2026 Redpanda Data.
 */

/**
 * Differential Fuzzer for seastar::sstring
 *
 * This fuzzer uses std::string as an oracle to find correctness bugs in sstring.
 * The fuzzer interprets the random input as a sequence of operations (opcodes),
 * applying each operation to both an sstring and a std::string in parallel.
 * After each operation, we verify that both strings remain equivalent.
 *
 * Architecture:
 * - NUM_SLOTS string pairs (sstring + std::string) for multi-string operations
 * - Each byte from fuzz input is interpreted as an opcode + parameters
 * - FuzzReader consumes bytes and generates deterministic test data from seeds
 * - Operations cover construction, mutation, search, comparison, and iteration
 *
 * The fuzzer is designed to exercise both the internal (small string optimization)
 * and external (heap-allocated) storage paths in sstring by using a mix of small
 * and large strings generated from the fuzz input.
 */

#include <seastar/core/sstring.hh>
#include <seastar/util/assert.hh>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>

using seastar::sstring;

// Helpers for C++20 compatibility (std::string::contains added in C++23)
bool contains(const std::string& str, std::string_view sv) {
    return str.find(sv) != std::string::npos;
}

bool contains(const std::string& str, char c) {
    return str.find(c) != std::string::npos;
}

// Command opcodes for the interpreted fuzzer
enum class Op : uint8_t {
    // Construction / Assignment
    ASSIGN_FROM_DATA = 0,   // Assign from inline data
    ASSIGN_COPY,            // Copy assignment from other slot
    ASSIGN_MOVE,            // Move assignment from other slot
    CLEAR,                  // Clear the string

    // Append operations
    APPEND_DATA,            // Append inline data
    APPEND_SSTRING,         // Append from another slot using +=
    APPEND_CHAR_N,          // Append N copies of character using resize

    // Size operations
    CHECK_SIZE,             // Verify size matches
    CHECK_EMPTY,            // Verify empty matches
    RESIZE,                 // Resize to N with fill char
    RESIZE_SHRINK,          // Resize smaller (relative to current size)

    // Search operations
    FIND_CHAR,              // Find character
    FIND_CHAR_POS,          // Find character starting at position
    FIND_SUBSTR,            // Find substring
    FIND_LAST_OF,           // Find last occurrence of character

    // Substring operations
    SUBSTR,                 // Extract substring
    ERASE,                  // Erase range using iterators

    // Comparison
    COMPARE,                // Compare with other slot
    COMPARE_SUBSTR,         // Compare substring

    // Predicates
    STARTS_WITH,            // Check prefix
    ENDS_WITH,              // Check suffix
    CONTAINS,               // Check contains

    // Access
    AT,                     // Bounds-checked access
    FRONT,                  // Access front element
    BACK,                   // Access back element

    // Multi-string operations
    SWAP,                   // Swap two slots
    CONCAT,                 // Concatenate two slots

    // Length operations
    LENGTH,                 // Check length() == size()

    // Equality/comparison operators
    EQUAL,                  // operator==
    NOT_EQUAL,              // operator!=
    LESS_THAN,              // operator<

    // Additional construction
    CONSTRUCT_FILL,         // Construct with (size, char)

    // Additional modification
    REPLACE,                // replace(pos, n1, s, n2)
    INSERT,                 // insert(p, beg, end)

    // Additional access
    SUBSCRIPT,              // operator[] unchecked access

    // Additional predicates (char variants)
    STARTS_WITH_CHAR,       // starts_with(char)
    ENDS_WITH_CHAR,         // ends_with(char)
    CONTAINS_CHAR,          // contains(char)

    // Additional find operations
    FIND_SSTRING,           // find(sstring, pos)
    FIND_LAST_OF_POS,       // find_last_of(char, pos)

    // Additional comparisons
    GREATER_THAN,           // operator>
    LESS_EQUAL,             // operator<=
    GREATER_EQUAL,          // operator>=

    // Conversion/access
    TO_STD_STRING,          // operator std::string()
    C_STR,                  // c_str() returns correct data
    DATA,                   // data() returns correct data

    // Iterator operations
    ITERATOR_DISTANCE,      // end() - begin() == size()
    REVERSE_ITERATE,        // iterate backwards and check

    // Additional construction
    CONSTRUCT_ITERATOR,     // construct from iterators

    // Raw data operations (bytes read directly from fuzz input)
    ASSIGN_RAW,             // Assign from raw fuzz input bytes
    APPEND_RAW,             // Append raw fuzz input bytes

    // Exception testing (out-of-bounds operations)
    AT_THROWING,            // at() with invalid position (must throw)
    SUBSTR_THROWING,        // substr() with invalid position (must throw)
    COMPARE_SUBSTR_THROWING, // compare() with invalid position (must throw)
    REPLACE_THROWING,       // replace() with invalid position (must throw)
    INSERT_THROWING,        // insert() with invalid iterator (must throw)

    OP_COUNT
};

// Number of string slots available for operations
constexpr size_t NUM_SLOTS = 4;

// Maximum size for any single string operation
// Making this too large reduces the effectiveness of the fuzzer at finding
// certain types of bugs, since most strings will be long so a bug like
// "the final byte is skipped in ==" is hard to catch since we are unlikely
// to reach that scenario with a longer string. However, we still want it
// be large enough to trigger all the code paths such as heap allocations,
// so this should be more than 2x the SSO limit.
constexpr size_t MAX_STRING_SIZE = 50;

// Knuth's MMIX LCG for deterministic data generation
// a = 6364136223846793005, c = 1442695040888963407, m = 2^64
using Rng = std::linear_congruential_engine<uint64_t, 6364136223846793005ULL, 1442695040888963407ULL, 0ULL>;

// Reader for consuming bytes from the fuzz input
class FuzzReader {
    const uint8_t* data_;
    size_t remaining_;

public:
    FuzzReader(const uint8_t* data, size_t size)
        : data_(data), remaining_(size) {}

    bool empty() const { return remaining_ == 0; }

    uint8_t read_u8() {
        if (remaining_ == 0) return 0;
        remaining_--;
        return *data_++;
    }

    uint16_t read_u16() {
        uint16_t lo = read_u8();
        uint16_t hi = read_u8();
        return lo | (hi << 8);
    }

    uint32_t read_u32() {
        uint32_t b0 = read_u8();
        uint32_t b1 = read_u8();
        uint32_t b2 = read_u8();
        uint32_t b3 = read_u8();
        return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
    }

    uint64_t read_u64() {
        uint64_t lo = read_u32();
        uint64_t hi = read_u32();
        return lo | (hi << 32);
    }

    size_t read_size(size_t max) {
        if (max == 0) return 0;
        if (max <= 255) {
            return read_u8() % (max + 1);
        }
        return read_u16() % (max + 1);
    }

    // Generate deterministic data from size and seed
    // Low entropy mode: characters alternate between two values
    // High entropy mode: full byte range
    std::string read_generated_data(size_t max_len) {
        size_t len = read_size(max_len);
        uint64_t seed = read_u64();
        int mode = seed % 3;  // 0 = zeros, 1 = low entropy, 2 = high entropy

        std::string result;
        result.reserve(len);

        if (mode == 0) {
            result.resize(len, '\0');
        } else if (mode == 1) {
            // Low entropy: only 2 possible character values
            // Helps exercise comparisons
            Rng rng(seed | 1);  // Ensure seed is never 0
            for (size_t i = 0; i < len; i++) {
                result.push_back((rng() & 1) ? 'a' : 'b');
            }
        } else {
            // High entropy: full byte range
            Rng rng(seed | 1);  // Ensure seed is never 0
            for (size_t i = 0; i < len; i++) {
                result.push_back(static_cast<char>(rng()));
            }
        }
        return result;
    }

    char read_char() {
        return static_cast<char>(read_u8());
    }

    // Read raw bytes directly from fuzz input
    std::string read_raw_data(size_t max_len) {
        size_t len = read_size(max_len);
        std::string result;
        result.reserve(len);
        for (size_t i = 0; i < len; i++) {
            result.push_back(static_cast<char>(read_u8()));
        }
        return result;
    }
};

// Verify that sstring and std::string are equivalent
void verify_equal(const sstring& ss, const std::string& rs) {
    SEASTAR_ASSERT(ss.size() == rs.size());
    SEASTAR_ASSERT(ss.empty() == rs.empty());
    SEASTAR_ASSERT(ss.size() == 0 || std::memcmp(ss.data(), rs.data(), ss.size()) == 0);
}

// Verify both implementations throw the same exception type
// Both MUST throw - this is for testing operations that should fail
// Op is a generic lambda that works on both sstring and std::string
template<typename Op>
void both_throw(sstring& ss, std::string& rs, Op&& op) {
    std::optional<std::type_index> ss_exception_type;
    std::optional<std::type_index> rs_exception_type;

    try {
        op(ss);
    } catch (const std::exception& e) {
        ss_exception_type = std::type_index(typeid(e));
    } catch (...) {
        ss_exception_type = std::type_index(typeid(void));
    }

    try {
        op(rs);
    } catch (const std::exception& e) {
        rs_exception_type = std::type_index(typeid(e));
    } catch (...) {
        rs_exception_type = std::type_index(typeid(void));
    }

    // Both must throw
    SEASTAR_ASSERT(ss_exception_type.has_value());
    SEASTAR_ASSERT(rs_exception_type.has_value());
    // Exception types must match
    SEASTAR_ASSERT(ss_exception_type == rs_exception_type);
}

// Execute the fuzzer program
void execute_program(FuzzReader& reader) {
    // The string slots under test
    sstring slots[NUM_SLOTS];
    std::string rss[NUM_SLOTS];

    while (!reader.empty()) {
        Op op = static_cast<Op>(reader.read_u8() % static_cast<uint8_t>(Op::OP_COUNT));
        uint8_t slot = reader.read_u8() % NUM_SLOTS;

        sstring& ss = slots[slot];
        std::string& rs = rss[slot];

        switch (op) {
            case Op::ASSIGN_FROM_DATA: {
                auto data = reader.read_generated_data(MAX_STRING_SIZE);
                ss = sstring(data.data(), data.size());
                rs = std::move(data);
                break;
            }

            case Op::ASSIGN_COPY: {
                uint8_t src = reader.read_u8() % NUM_SLOTS;
                ss = slots[src];
                rs = rss[src];
                break;
            }

            case Op::ASSIGN_MOVE: {
                uint8_t src = reader.read_u8() % NUM_SLOTS;
                if (src != slot) {
                    ss = std::move(slots[src]);
                    rs = std::move(rss[src]);
                    // Reset moved-from state to known empty
                    slots[src] = sstring();
                    rss[src] = std::string();
                }
                break;
            }

            case Op::CLEAR: {
                ss = sstring();
                rs.clear();
                break;
            }

            case Op::APPEND_DATA: {
                auto data = reader.read_generated_data(MAX_STRING_SIZE);
                if (ss.size() + data.size() <= MAX_STRING_SIZE) {
                    ss.append(data.data(), data.size());
                    rs.append(data.data(), data.size());
                }
                break;
            }

            case Op::APPEND_SSTRING: {
                uint8_t src = reader.read_u8() % NUM_SLOTS;
                if (ss.size() + slots[src].size() <= MAX_STRING_SIZE) {
                    ss += slots[src];
                    rs += rss[src];
                }
                break;
            }

            case Op::APPEND_CHAR_N: {
                char c = reader.read_char();
                size_t max_count = MAX_STRING_SIZE - ss.size();
                size_t count = reader.read_size(max_count);
                // sstring doesn't have append(count, char), use resize instead
                size_t old_size = ss.size();
                ss.resize(old_size + count, c);
                rs.resize(old_size + count, c);
                break;
            }

            case Op::CHECK_SIZE: {
                SEASTAR_ASSERT(ss.size() == rs.size());
                break;
            }

            case Op::CHECK_EMPTY: {
                SEASTAR_ASSERT(ss.empty() == rs.empty());
                break;
            }

            case Op::RESIZE: {
                size_t new_size = reader.read_size(MAX_STRING_SIZE);
                if (new_size <= MAX_STRING_SIZE) {
                    char fill = reader.read_char();
                    ss.resize(new_size, fill);
                    rs.resize(new_size, fill);
                }
                break;
            }

            case Op::RESIZE_SHRINK: {
                size_t new_size = reader.read_size(ss.size());
                ss.resize(new_size);
                rs.resize(new_size);
                break;
            }

            case Op::FIND_CHAR: {
                char c = reader.read_char();
                size_t r1 = ss.find(c);
                size_t r2 = rs.find(c);
                SEASTAR_ASSERT(r1 == r2);
                break;
            }

            case Op::FIND_CHAR_POS: {
                char c = reader.read_char();
                size_t pos = reader.read_size(ss.size() + 10);  // Allow some out of bounds
                size_t r1 = ss.find(c, pos);
                size_t r2 = rs.find(c, pos);
                SEASTAR_ASSERT(r1 == r2);
                break;
            }

            case Op::FIND_SUBSTR: {
                uint8_t src = reader.read_u8() % NUM_SLOTS;
                std::string_view sv(rss[src]);
                size_t r1 = ss.find(sv);
                size_t r2 = rs.find(rss[src]);
                SEASTAR_ASSERT(r1 == r2);
                break;
            }

            case Op::FIND_LAST_OF: {
                char c = reader.read_char();
                size_t r1 = ss.find_last_of(c);
                size_t r2 = rs.find_last_of(c);
                SEASTAR_ASSERT(r1 == r2);
                break;
            }

            case Op::SUBSTR: {
                size_t pos = reader.read_size(ss.size());
                size_t max_len = ss.size() >= pos ? ss.size() - pos : 0;
                size_t len = reader.read_size(max_len);
                sstring sub1 = ss.substr(pos, len);
                std::string sub2 = rs.substr(pos, len);
                verify_equal(sub1, sub2);
                break;
            }

            case Op::ERASE: {
                if (!ss.empty()) {
                    size_t pos = reader.read_size(ss.size() - 1);
                    size_t max_len = ss.size() - pos;
                    size_t len = reader.read_size(max_len);
                    // sstring::erase uses iterators
                    ss.erase(ss.begin() + pos, ss.begin() + pos + len);
                    rs.erase(pos, len);
                }
                break;
            }

            case Op::COMPARE: {
                uint8_t other = reader.read_u8() % NUM_SLOTS;
                // Compare sign only, not magnitude
                std::string_view sv(rss[other]);
                int r1 = ss.compare(sv);
                int r2 = rs.compare(rss[other]);
                SEASTAR_ASSERT((r1 < 0) == (r2 < 0) && (r1 == 0) == (r2 == 0));
                break;
            }

            case Op::COMPARE_SUBSTR: {
                uint8_t other = reader.read_u8() % NUM_SLOTS;
                size_t pos = reader.read_size(ss.size());
                size_t max_len = ss.size() >= pos ? ss.size() - pos : 0;
                size_t len = reader.read_size(max_len);
                std::string_view sv(rss[other]);
                int r1 = ss.compare(pos, len, sv);
                int r2 = rs.compare(pos, len, rss[other]);
                SEASTAR_ASSERT((r1 < 0) == (r2 < 0) && (r1 == 0) == (r2 == 0));
                break;
            }

            case Op::STARTS_WITH: {
                uint8_t other = reader.read_u8() % NUM_SLOTS;
                std::string_view sv(rss[other]);
                bool r1 = ss.starts_with(sv);
                bool r2 = rs.starts_with(sv);
                SEASTAR_ASSERT(r1 == r2);
                break;
            }

            case Op::ENDS_WITH: {
                uint8_t other = reader.read_u8() % NUM_SLOTS;
                std::string_view sv(rss[other]);
                bool r1 = ss.ends_with(sv);
                bool r2 = rs.ends_with(sv);
                SEASTAR_ASSERT(r1 == r2);
                break;
            }

            case Op::CONTAINS: {
                uint8_t other = reader.read_u8() % NUM_SLOTS;
                std::string_view sv(rss[other]);
                bool r1 = ss.contains(sv);
                bool r2 = contains(rs, sv);
                SEASTAR_ASSERT(r1 == r2);
                break;
            }

            case Op::AT: {
                if (!ss.empty()) {
                    size_t pos = reader.read_size(ss.size() - 1);
                    char c1 = ss.at(pos);
                    char c2 = rs.at(pos);
                    SEASTAR_ASSERT(c1 == c2);
                }
                break;
            }

            case Op::FRONT: {
                if (!ss.empty()) {
                    SEASTAR_ASSERT(ss.front() == rs.front());
                }
                break;
            }

            case Op::BACK: {
                if (!ss.empty()) {
                    SEASTAR_ASSERT(ss.back() == rs.back());
                }
                break;
            }

            case Op::SWAP: {
                uint8_t other = reader.read_u8() % NUM_SLOTS;
                ss.swap(slots[other]);
                rs.swap(rss[other]);
                break;
            }

            case Op::CONCAT: {
                uint8_t other = reader.read_u8() % NUM_SLOTS;
                if (ss.size() + slots[other].size() <= MAX_STRING_SIZE) {
                    ss = ss + slots[other];
                    rs = rs + rss[other];
                }
                break;
            }

            case Op::LENGTH: {
                // Verify length() == size()
                SEASTAR_ASSERT(ss.length() == ss.size());
                SEASTAR_ASSERT(ss.length() == rs.length());
                break;
            }

            case Op::EQUAL: {
                uint8_t other = reader.read_u8() % NUM_SLOTS;
                bool r1 = (ss == slots[other]);
                bool r2 = (rs == rss[other]);
                SEASTAR_ASSERT(r1 == r2);
                break;
            }

            case Op::NOT_EQUAL: {
                uint8_t other = reader.read_u8() % NUM_SLOTS;
                bool r1 = (ss != slots[other]);
                bool r2 = (rs != rss[other]);
                SEASTAR_ASSERT(r1 == r2);
                break;
            }

            case Op::LESS_THAN: {
                uint8_t other = reader.read_u8() % NUM_SLOTS;
                bool r1 = (ss < slots[other]);
                bool r2 = (rs < rss[other]);
                SEASTAR_ASSERT(r1 == r2);
                break;
            }

            case Op::CONSTRUCT_FILL: {
                size_t len = reader.read_size(MAX_STRING_SIZE);
                char c = reader.read_char();
                ss = sstring(len, c);
                rs = std::string(len, c);
                break;
            }

            case Op::REPLACE: {
                size_t pos = reader.read_size(ss.size());
                size_t max_n1 = ss.size() >= pos ? ss.size() - pos : 0;
                size_t n1 = reader.read_size(max_n1);
                auto data = reader.read_generated_data(MAX_STRING_SIZE);
                size_t n2 = data.size();
                // Check resulting size won't exceed max
                if (ss.size() - n1 + n2 <= MAX_STRING_SIZE) {
                    ss.replace(pos, n1, data.data(), n2);
                    rs.replace(pos, n1, data.data(), n2);
                }
                break;
            }

            case Op::INSERT: {
                size_t pos = reader.read_size(ss.size());
                auto data = reader.read_generated_data(MAX_STRING_SIZE);
                if (ss.size() + data.size() <= MAX_STRING_SIZE) {
                    ss.insert(ss.begin() + pos, data.begin(), data.end());
                    rs.insert(rs.begin() + pos, data.begin(), data.end());
                }
                break;
            }

            case Op::SUBSCRIPT: {
                if (!ss.empty()) {
                    size_t pos = reader.read_size(ss.size() - 1);
                    SEASTAR_ASSERT(ss[pos] == rs[pos]);
                }
                break;
            }

            case Op::STARTS_WITH_CHAR: {
                char c = reader.read_char();
                SEASTAR_ASSERT(ss.starts_with(c) == rs.starts_with(c));
                break;
            }

            case Op::ENDS_WITH_CHAR: {
                char c = reader.read_char();
                SEASTAR_ASSERT(ss.ends_with(c) == rs.ends_with(c));
                break;
            }

            case Op::CONTAINS_CHAR: {
                char c = reader.read_char();
                SEASTAR_ASSERT(ss.contains(c) == contains(rs, c));
                break;
            }

            case Op::FIND_SSTRING: {
                uint8_t src = reader.read_u8() % NUM_SLOTS;
                size_t pos = reader.read_size(ss.size() + 10);
                SEASTAR_ASSERT(ss.find(slots[src], pos) == rs.find(rss[src], pos));
                break;
            }

            case Op::FIND_LAST_OF_POS: {
                char c = reader.read_char();
                size_t pos = reader.read_size(ss.size() + 10);
                SEASTAR_ASSERT(ss.find_last_of(c, pos) == rs.find_last_of(c, pos));
                break;
            }

            case Op::GREATER_THAN: {
                uint8_t other = reader.read_u8() % NUM_SLOTS;
                SEASTAR_ASSERT((ss > slots[other]) == (rs > rss[other]));
                break;
            }

            case Op::LESS_EQUAL: {
                uint8_t other = reader.read_u8() % NUM_SLOTS;
                SEASTAR_ASSERT((ss <= slots[other]) == (rs <= rss[other]));
                break;
            }

            case Op::GREATER_EQUAL: {
                uint8_t other = reader.read_u8() % NUM_SLOTS;
                SEASTAR_ASSERT((ss >= slots[other]) == (rs >= rss[other]));
                break;
            }

            case Op::TO_STD_STRING: {
                std::string converted = static_cast<std::string>(ss);
                SEASTAR_ASSERT(converted == rs);
                break;
            }

            case Op::C_STR: {
                // Use strcmp to verify null termination is correct
                SEASTAR_ASSERT(std::strcmp(ss.c_str(), rs.c_str()) == 0);
                break;
            }

            case Op::DATA: {
                SEASTAR_ASSERT(ss.size() == rs.size());
                SEASTAR_ASSERT(std::memcmp(ss.data(), rs.data(), ss.size()) == 0);
                break;
            }

            case Op::ITERATOR_DISTANCE: {
                auto dist = ss.end() - ss.begin();
                SEASTAR_ASSERT(static_cast<size_t>(dist) == ss.size());
                SEASTAR_ASSERT(static_cast<size_t>(dist) == rs.size());
                break;
            }

            case Op::REVERSE_ITERATE: {
                // Iterate backwards and compare
                auto it1 = ss.end();
                auto it2 = rs.end();
                while (it1 != ss.begin()) {
                    --it1;
                    --it2;
                    SEASTAR_ASSERT(*it1 == *it2);
                }
                break;
            }

            case Op::CONSTRUCT_ITERATOR: {
                // Construct from rs's iterators
                if (rs.size() <= MAX_STRING_SIZE) {
                    ss = sstring(rs.begin(), rs.end());
                    // rs stays the same, s should now equal rs
                }
                break;
            }

            case Op::ASSIGN_RAW: {
                auto data = reader.read_raw_data(MAX_STRING_SIZE);
                ss = sstring(data.data(), data.size());
                rs = std::move(data);
                break;
            }

            case Op::APPEND_RAW: {
                auto data = reader.read_raw_data(MAX_STRING_SIZE);
                if (ss.size() + data.size() <= MAX_STRING_SIZE) {
                    ss.append(data.data(), data.size());
                    rs.append(data.data(), data.size());
                }
                break;
            }

            case Op::AT_THROWING: {
                // Force position >= size() to guarantee exception
                size_t pos = ss.size() + reader.read_size(10);
                both_throw(ss, rs, [pos](auto& s) { (void)s.at(pos); });
                break;
            }

            case Op::SUBSTR_THROWING: {
                // Force from > size() to guarantee exception
                size_t from = ss.size() + 1 + reader.read_size(10);
                size_t len = reader.read_size(MAX_STRING_SIZE);
                both_throw(ss, rs, [from, len](auto& s) { (void)s.substr(from, len); });
                break;
            }

            case Op::COMPARE_SUBSTR_THROWING: {
                uint8_t other = reader.read_u8() % NUM_SLOTS;
                // Force pos > size() to guarantee exception
                size_t pos = ss.size() + 1 + reader.read_size(10);
                size_t len = reader.read_size(MAX_STRING_SIZE);
                // Work around libstdc++ bug: string_view overload of compare()
                // has incorrect noexcept, causing terminate() instead of throwing.
                // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=123991
                both_throw(ss, rs, [pos, len, &slots, &rss, other](auto& s) {
                    if constexpr (std::is_same_v<std::decay_t<decltype(s)>, sstring>) {
                        (void)s.compare(pos, len, std::string_view(slots[other]));
                    } else {
                        (void)s.compare(pos, len, rss[other]);
                    }
                });
                break;
            }

            case Op::REPLACE_THROWING: {
                // Force pos > size() to guarantee exception
                size_t pos = ss.size() + 1 + reader.read_size(10);
                size_t n1 = reader.read_size(MAX_STRING_SIZE);
                auto data = reader.read_generated_data(MAX_STRING_SIZE);
                both_throw(ss, rs, [pos, n1, &data](auto& s) {
                    s.replace(pos, n1, data.data(), data.size());
                });
                break;
            }

            case Op::INSERT_THROWING: {
                // Force position > size() to guarantee exception
                size_t pos = ss.size() + 1 + reader.read_size(10);
                auto data = reader.read_generated_data(MAX_STRING_SIZE);
                both_throw(ss, rs, [pos, &data](auto& s) {
                    s.insert(s.begin() + pos, data.begin(), data.end());
                });
                break;
            }

            default:
                SEASTAR_ASSERT(false);  // Invalid op
            }

        // Verify consistency after every operation
        verify_equal(ss, rs);
    }

    // Final verification of all slots
    for (size_t i = 0; i < NUM_SLOTS; i++) {
        verify_equal(slots[i], rss[i]);
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Skip empty inputs
    if (size == 0) {
        return 0;
    }

    FuzzReader reader(data, size);
    execute_program(reader);

    return 0;
}
