// A shard's keys: a hash table made of many small tables, each of which
// grows on its own (version 12).
#pragma once
#include <seastar/core/prefetch.hh>
#include <seastar/core/sstring.hh>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

using namespace seastar;

// snippet: key-string
// A key, with up to 22 characters kept inline, in the table's own node,
// rather than in an allocation of their own: Redis keys are often longer
// than sstring's 14. A key_string takes 24 bytes: key_inline_size counts
// the null that ends the characters.
constexpr size_t key_inline_size = 23;
using key_string = basic_sstring<char, uint32_t, key_inline_size>;
// end snippet: key-string

// snippet: key-table
// The keys of a shard, and their values: a directory of small hash tables,
// chosen by the first bits of a key's hash. A small table with max_slots
// slots that is three quarters full splits in two, by the next bit, so no
// growth moves more than one small table's keys, and none needs a large
// allocation. A small
// table is an array of slots, each with a key's hash and a pointer to its
// node, which holds the key and the value, so a lookup reads one slot and
// one node: two cache misses, when the table is too big for the cache.
template <typename Value>
class key_table {
public:
    using value_type = std::pair<const key_string, Value>;
private:
    static constexpr size_t max_slots = 4096;   // in a small table
    struct slot {
        uint64_t hash;
        value_type* node;                       // null: an empty slot
    };
    struct small_table {
        std::unique_ptr<slot[]> slots;
        uint32_t mask;      // the number of slots, minus one
        uint32_t size;      // the number of keys
        unsigned depth;     // how many of the hash's first bits it covers
    };
    // The small tables, and the directory: 2^_depth entries, the indexes of
    // small tables; a small table of depth d fills 2^(_depth - d) of them,
    // in a row. The small tables' headers sit together, in one vector,
    // which is small enough to stay in the cache.
    std::vector<small_table> _tables;
    std::vector<uint32_t> _directory;
    unsigned _depth = 0;
    size_t _size = 0;
    // end snippet: key-table

    small_table& table_of(uint64_t hash) {
        return _tables[_directory[_depth ? hash >> (64 - _depth) : 0]];
    }
    // snippet: probe
    // The slot that has this hash and key, or the empty slot where it
    // would go: probing goes on to the next slot, as long as it's taken.
    static size_t probe(const small_table& t, uint64_t hash, std::string_view key) {
        for (size_t i = hash & t.mask; ; i = (i + 1) & t.mask) {
            auto& s = t.slots[i];
            if (!s.node || (s.hash == hash && s.node->first == key)) {
                return i;
            }
        }
    }
    // end snippet: probe
    static small_table make_table(size_t slots, unsigned depth) {
        auto p = std::make_unique<slot[]>(slots);
        return small_table{std::move(p), uint32_t(slots - 1), 0, depth};
    }
    static void insert_slot(small_table& t, slot s) {
        size_t i = s.hash & t.mask;
        while (t.slots[i].node) {
            i = (i + 1) & t.mask;
        }
        t.slots[i] = s;
        t.size++;
    }
    void grow(small_table& t);
    void split(uint32_t index, uint64_t hash);
public:
    static uint64_t hash_of(std::string_view key) {
        // std::hash's low bits choose the shard (see owner()), so all the
        // keys of a shard share them: mix every bit into every other, since
        // the high bits choose the small table, and the low bits the slot.
        uint64_t h = std::hash<std::string_view>()(key);
        h ^= h >> 32;
        h *= 0x9e3779b97f4a7c15ull;
        h ^= h >> 29;
        return h;
    }
    // An iterator to one element.
    class iterator {
        value_type* _node = nullptr;
        friend class key_table;
        explicit iterator(value_type* n) : _node(n) {}
    public:
        iterator() = default;
        value_type& operator*() const { return *_node; }
        value_type* operator->() const { return _node; }
        bool operator==(const iterator&) const = default;
    };

    key_table() {
        _tables.push_back(make_table(16, 0));
        _directory.push_back(0);
    }
    key_table(key_table&&) noexcept = default;
    key_table& operator=(key_table&& o) noexcept {
        // o, which gets this table's old contents, destroys them.
        std::swap(_tables, o._tables);
        std::swap(_directory, o._directory);
        std::swap(_depth, o._depth);
        std::swap(_size, o._size);
        return *this;
    }
    ~key_table() {
        for (auto& t : _tables) {
            for (size_t i = 0; t.slots && i <= t.mask; i++) {
                delete t.slots[i].node;
            }
        }
    }

    size_t size() const { return _size; }
    bool empty() const { return _size == 0; }
    iterator end() const { return iterator(); }

    // snippet: find
    iterator find(std::string_view key) {
        auto h = hash_of(key);
        auto& t = table_of(h);
        return iterator(t.slots[probe(t, h, key)].node);
    }
    // end snippet: find
    // snippet: sample
    // A key chosen at random, or end() if the table is empty: a random
    // small table, and in it, the first key from a random slot on.
    template <typename Random>
    iterator sample(Random& random) {
        if (_size == 0) {
            return end();
        }
        while (true) {
            auto& t = _tables[random() % _tables.size()];
            if (t.size == 0) {
                continue;
            }
            for (size_t i = random() & t.mask; ; i = (i + 1) & t.mask) {
                if (t.slots[i].node) {
                    return iterator(t.slots[i].node);
                }
            }
        }
    }
    // end snippet: sample
    // snippet: prefetch
    // For a key that will be looked up soon: ask the CPU to start loading
    // the slot where the key would be, without waiting for it...
    void prefetch_slot(std::string_view key) {
        auto h = hash_of(key);
        auto& t = table_of(h);
        prefetch(&t.slots[h & t.mask]);
    }
    // ... and, a little later, when the slot is likely in the cache, the
    // node that the slot points to.
    void prefetch_node(std::string_view key) {
        auto h = hash_of(key);
        auto& t = table_of(h);
        if (auto* node = t.slots[h & t.mask].node) {
            prefetch(node);
        }
    }
    // end snippet: prefetch
    // snippet: try-emplace
    // Finds the key, or adds it with a default value; returns it, and
    // whether it was added.
    std::pair<iterator, bool> try_emplace(std::string_view key) {
        auto h = hash_of(key);
        auto* t = &table_of(h);
        auto i = probe(*t, h, key);
        if (auto* n = t->slots[i].node) {
            return {iterator(n), false};
        }
        if (t->size + 1 > (t->mask + 1) / 4 * 3) {
            // Full enough: grow it, or split it, and look again.
            if (t->mask + 1 < max_slots) {
                grow(*t);
            } else {
                split(_directory[_depth ? h >> (64 - _depth) : 0], h);
            }
            t = &table_of(h);
            i = probe(*t, h, key);
        }
        auto* n = new value_type(key_string(key), Value());
        t->slots[i] = slot{h, n};
        t->size++;
        _size++;
        return {iterator(n), true};
    }
    // end snippet: try-emplace
    void erase(iterator it) {
        auto& key = it->first;
        auto h = hash_of(key);
        auto& t = table_of(h);
        auto i = probe(t, h, key);
        delete t.slots[i].node;
        t.slots[i].node = nullptr;
        t.size--;
        _size--;
        // Move back the slots that follow, which may have probed past this
        // one, so that a lookup never stops early at the hole.
        for (size_t j = (i + 1) & t.mask; t.slots[j].node; j = (j + 1) & t.mask) {
            auto s = t.slots[j];
            t.slots[j].node = nullptr;
            t.size--;
            insert_slot(t, s);
        }
    }
    // Erases up to n keys, in no particular order, and returns how many,
    // for destroying a table gradually: after it, the table may only be
    // erased further, asked whether it's empty, and destroyed.
    size_t erase_some(size_t n) {
        size_t erased = 0;
        while (erased < n && !_tables.empty()) {
            auto& t = _tables.back();
            for (size_t i = 0; erased < n && t.size && i <= t.mask; i++) {
                if (auto* node = std::exchange(t.slots[i].node, nullptr)) {
                    delete node;
                    t.size--;
                    _size--;
                    erased++;
                }
            }
            if (!t.size) {
                _tables.pop_back();
            }
        }
        return erased;
    }
    void clear() {
        *this = key_table();
    }
    // snippet: scan
    // Calls f(element) for every element of one small table, the one at
    // position, a hash. Returns the position of the next small table, or 0
    // after the last one. A scan that calls scan() again with the returned
    // position, even after the table changed, visits every key that was
    // there all along, once.
    template <typename F>
    size_t scan(size_t position, F f) {
        auto& t = table_of(position);
        auto depth = t.depth;
        for (size_t i = 0; i <= t.mask; i++) {
            if (i + 8 <= t.mask && t.slots[i + 8].node) {
                prefetch(t.slots[i + 8].node);
            }
            if (t.slots[i].node) {
                f(*t.slots[i].node);
            }
        }
        if (depth == 0) {
            return 0;
        }
        auto shift = 64 - depth;
        return ((position >> shift) + 1) << shift;  // 0 after the last one
    }
    // end snippet: scan
};

// Doubles a small table's slots, which then hold the same keys.
template <typename Value>
void key_table<Value>::grow(small_table& t) {
    auto bigger = make_table((t.mask + 1) * 2, t.depth);
    for (size_t i = 0; i <= t.mask; i++) {
        if (t.slots[i].node) {
            insert_slot(bigger, t.slots[i]);
        }
    }
    t = std::move(bigger);
}

// snippet: split
// Splits a small table, which is full, in two, by the next bit of the
// hash: the slots of the keys whose bit is 1 move to a new small table. The
// nodes don't move, and their addresses stay the same.
template <typename Value>
void key_table<Value>::split(uint32_t index, uint64_t hash) {
    if (_tables[index].depth == _depth) {
        // The directory has one entry for this table: double it.
        std::vector<uint32_t> bigger(_directory.size() * 2);
        for (size_t i = 0; i < _directory.size(); i++) {
            bigger[2 * i] = bigger[2 * i + 1] = _directory[i];
        }
        _directory = std::move(bigger);
        _depth++;
    }
    auto& t = _tables[index];
    auto bit = uint64_t(1) << (63 - t.depth);
    auto depth = t.depth + 1;
    auto zeros = make_table(t.mask + 1, depth);
    auto ones = make_table(t.mask + 1, depth);
    for (size_t i = 0; i <= t.mask; i++) {
        if (t.slots[i].node) {
            insert_slot(t.slots[i].hash & bit ? ones : zeros, t.slots[i]);
        }
    }
    t = std::move(zeros);
    _tables.push_back(std::move(ones));
    // The directory entries of the table's range whose bit is 1 now point
    // to the new table.
    uint32_t new_index = _tables.size() - 1;
    size_t first = (hash >> (64 - _depth)) & ~((size_t(1) << (_depth - depth + 1)) - 1);
    size_t half = size_t(1) << (_depth - depth);
    for (size_t i = first + half; i < first + 2 * half; i++) {
        _directory[i] = new_index;
    }
}
// end snippet: split
