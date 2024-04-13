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
 * DB server demo
 * Author: AlexanderMihail@gmail.com
 * Incept Date: 2024.04.08
 * Modified on: 2024.04.12
 *
 * A simple persistent key/value store
 * Utilizing the seastar library for asynchronous execution
 * and input/output operations, maximizing the usage of the
 * disk CPU and memory.
 * Dataset:
 * - Keys are utf-8 strings up to 255 bytes in length
 * - Values are utf-8 strings of unlimited length.
 * - Data is sharded among the CPU cores, based on they keys
 * Data is manipulated and accessed using http-based REST API:
 * - Insert / update of a single key/value pair
 * - Query of a single key, returning its value
 * - Query the sorted list of all keys
 * - Delete a single key (and its associated value)
 * Cache:
 * - Newly written data should be cached in memory
 * - Recently used data should be cached in memory
 * - The size of the cache is limited to some configurable limit
 * - Least recently used data should be evicted from the cache to
 *   make room for new data that was recently read or written
 */
/*
 * Implementation details:
 * - Data is stored on disk in two files with names <tablename>_keys.txt and <tablename>_values.txt.
 * - Data is text so that it may be inspected and polled in Notepad++.
 * - The keys.txt (index) file is a fixed record length of some 289 bytes. The fixed record length is to allow instant updates of key values.
 * - The values.txt (data) file is a concatenation of values with no separation. The index file has each key referencing its value by file offset and size of value.
 * - The memory cache consists of keys and values deques, plus the xref map of (key_name,deque_element) pair. The xref is used for O(logN) retrieval.
 * - The memory cache has to be at least 1 element in size. Eviction happens at the right-end of the key/value deques.
 * - Last used elemets are dislocated and appended to the left of the deques. So the left-to-right order is youngest-to-oldest.
 * - For performance reasons, resizing the value of a key is recorded in-place in the values data file if the new size is less or equal to the previous value.
 * - The growing value is recorded at the end of the data file so to avoid merging and rebuilding the index file. A gap is now present in the value file where the value used to be prior to its growth.
 * - The size of the value file can be compared to the Stats::gaps profile to decide if a compaction should be initiated.
 * - All read/write operations on the data set are protected by a spinlock. Pass/Collision ratio is monitored.
 * - For performance reasons, deleted keys are marked and kept in the index file for lazy compaction. Searching for keys are constantly reducing deleted keys.
 * Limitations:
 * - To keep the index human-readable, a CSV format was chosen: (key-name,off_values,size_value, padding).
 * - This format implies that keys must not contain comma characters.
 * - The empty string in queries has special meaning. Keys cannot be empty, values can.
 */

#include <seastar/http/httpd.hh>
#include <seastar/http/handlers.hh>
#include <seastar/http/function_handlers.hh>
#include <seastar/http/file_handler.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/reactor.hh>
#include <seastar/http/api_docs.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/prometheus.hh>
#include <seastar/core/print.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/util/defer.hh>

#pragma GCC diagnostic ignored "-Wunused-variable"

namespace bpo = boost::program_options;

using namespace seastar;
using namespace httpd;

const char*VERSION="Demo Database Server 0.3";
const char* available_trace_levels[] = {"LOCKS", "FILEIO", "CACHE", "SERVER"};
enum ETrace { LOCKS, FILEIO, CACHE, SERVER, MAX};
int trace_level = (1 << ETrace::FILEIO) | (1 << ETrace::CACHE) | (1 << ETrace::SERVER);
logger applog("app");

// Tried to use seastar::coroutine::experimental::generator<Index>.
// Resorted to adopting the following ugly construct for the index revolver coroutine.
// Generator should be part of std, or be a compiler generated construct.
// C++ coroutines are trully a mess.

template<typename T>
struct Generator
{
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;
    struct promise_type // required
    {
        Generator get_return_object() // This is fundamental
        {
            return Generator(handle_type::from_promise(*this));
        }
        std::suspend_always initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void unhandled_exception() { exception_ = std::current_exception(); } // saving
        // exception
        template<std::convertible_to<T> From> // C++20 concept
        std::suspend_always yield_value(From&& from)
        {
            value_ = std::forward<From>(from); // caching the result in promise
            return {};
        }
        void return_void() {}
    private:
        friend struct Generator;
        T value_;
        std::exception_ptr exception_;
    };
    Generator(handle_type h) : h_(h) {}
    ~Generator() { h_.destroy(); }
    explicit operator bool()
    {
        fill();
        return !h_.done();
    }
    T operator()()
    {
        fill();
        full_ = false; // we are going to move out previously cached
            // result to make promise empty again
        return std::move(h_.promise().value_);
    }
private:
    handle_type h_;
    bool full_ = false;
    void fill()
    {
        if (!full_)
        {
            h_();
            if (h_.promise().exception_)
                std::rethrow_exception(h_.promise().exception_);
            // propagate coroutine exception in called context
            full_ = true;
        }
    }
};

struct Lock
{
    std::atomic_bool& value;
    bool reentered;
    Lock(std::atomic_bool& value, bool reentered=false) : value(value), reentered(reentered)
    {
        if (reentered)
            return;
        bool expected = false;
        while (!atomic_compare_exchange_strong(&value, &expected, true))
            collisions++;
        passes++;
        if (trace_level & (1 << ETrace::LOCKS)) applog.info("Locked");
    }
    ~Lock()
    {
        if (reentered)
            return;
        if (trace_level & (1 << ETrace::LOCKS)) applog.info("Unlocked");
        value = false;
    }
    static std::atomic_int passes;
    static std::atomic_int collisions;
};
std::atomic_int Lock::passes = 0;
std::atomic_int Lock::collisions = 0;

struct Stats
{
    static std::atomic_int requests, creates, drops;
    std::atomic_int inserts{0}, deletes{0}, updates{0}, gets{0}, purges{0}, invalidates{0}, evictions{0}, compacts{0};
    operator sstring() const
    {
        return format(
            "requests:{}\n"
            "creates:{}\n"
            "drops:{}\n"
            "inserts:{}\n"
            "deletes:{}\n"
            "updates:{}\n"
            "gets:{}\n"
            "purges:{}\n"
            "invalidates:{}\n"
            "evictions:{}\n"
            "compacts:{}\n"
            ,
            requests,
            creates,
            drops,
            inserts,
            deletes,
            updates,
            gets,
            purges,
            invalidates,
            evictions,
            compacts
        );
    }
};
std::atomic_int Stats::requests = 0;
std::atomic_int Stats::creates = 0;
std::atomic_int Stats::drops = 0;

struct Table : Stats {
    std::string name;
    size_t max_cache {2};

    constexpr static auto db_prefix = "db_";
    constexpr static auto keys_fname = "_keys.txt";
    constexpr static auto values_fname = "_values.txt";
    constexpr static auto compact_fname = "_compact.txt";
    constexpr static auto master_table_name = "Master";

    static std::map<std::string, Table*> tables;

    struct Value
    {
        std::string text;
        long long fpos;
        size_t sz;
    };
    struct Key
    {
        std::string text;
        long long fpos;
        std::deque<Value>::iterator it_value;
    };
    struct Index { int index; char key[256]; off_t vfpos; size_t vsz; };

    Table(Table const &) = delete;
    void operator=(Table const &) = delete;
    explicit Table(std::string name) : name(name)
    {
        fid_keys = open((db_prefix+name+keys_fname).c_str(), O_CREAT | O_RDWR, S_IREAD|S_IWRITE|S_IRGRP|S_IROTH);
        applog.info("Opened {:} returned {:}, {:}", db_prefix+name+keys_fname, fid_keys, errno);
        fid_values = open((db_prefix+name+values_fname).c_str(), O_CREAT | O_RDWR, S_IREAD|S_IWRITE|S_IRGRP|S_IROTH);
        applog.info("Opened {:} returned {:}", name+keys_fname, fid_values, errno);
        creates++;
    }
    std::string close(bool remove_files)
    {
        if (fid_keys>0)
            ::close(fid_keys);
        if (fid_values>0)
            ::close(fid_values);
        if (!remove_files)
            return "Closed files " + (db_prefix+name+keys_fname) + " and " + (db_prefix+name+values_fname) + "\n";
        drops++;
        unlink((db_prefix+name+keys_fname).c_str());
        unlink((db_prefix+name+values_fname).c_str());
        unlink((db_prefix+name+compact_fname).c_str());
        return "Removed files " + (db_prefix+name+keys_fname) + " and " + (db_prefix+name+values_fname) + "\n";
    }
    std::string purge(bool files)
    {
        Lock l(lock_cache);
        values.clear();
        keys.clear();
        xref.clear();
        std::string s="";
        if (files)
        {
            s = "Purged cache and files";
            entries = 0;
            spaces = 0;
            ftruncate(fid_values, 0);
            ftruncate(fid_keys, 0);
            purges++;
        }
        else
            s = "Purged cache";
        invalidates++;
        if (trace_level & ((1 << ETrace::CACHE) | (1 << ETrace::FILEIO)))
            applog.info("purge: cache{:}", files ? " and files" : "");
        return s;
    }
    std::string compact()
    {
        Lock l(lock_cache);
        std::string s = purge(false);
        int problems = 0;
        auto fid_temp = open((db_prefix+name+compact_fname).c_str(), O_CREAT | O_TRUNC | O_RDWR /*| O_TMPFILE*/, S_IREAD|S_IWRITE|S_IRGRP|S_IROTH);
        if (fid_temp>0)
        {
            off_t fpos_keys = 0;
            off_t fpos_values = 0;
            off_t fpos_temp = 0;
            char buf[key_len];
            lseek(fid_keys, 0, SEEK_SET);
            while (true)
            {
                auto len = read(fid_keys, buf, sizeof(buf)-1);
                if (len==0)
                    break; // end of index file
                if (len<0)
                {
                    problems |= (1<<0);
                    break;
                }
                char * sp = strchr(buf,',');
                *sp++=0;
                size_t sz;
                sscanf(sp, "%jd,%zu", &fpos_values, &sz);
                if (fpos_values==index_deleted_marker)
                {
                    auto len = read(fid_keys, buf, sizeof(buf)-1);
                    if (len==0)
                    {
                        ftruncate(fid_keys, fpos_keys);
                        break; // end of index file
                    }
                    lseek(fid_keys, fpos_keys, SEEK_SET);
                    write(fid_keys, buf, sizeof(buf)-1); // Shift the next index entry up by one.
                    continue; // index key is marked as deleted
                }
                std::string val(sz+1, 0);
                lseek(fid_values, fpos_values, SEEK_SET);
                read(fid_values, (char*)val.c_str(), sz);
                write(fid_temp, val.c_str(), sz);
                sprintf(buf+strlen(buf), ",%jd,%zu,", fpos_temp, sz);
                len = strlen(buf);
                auto pad = sizeof(buf)-1-len-1;
                sprintf(buf+len, "%*c\n", int(pad), ' ');
                lseek(fid_keys, fpos_keys, SEEK_SET);
                write(fid_keys, buf, sizeof(buf)-1);
                fpos_keys += sizeof(buf)-1;
                fpos_temp += sz;
            }
            ftruncate(fid_keys, fpos_keys);
            lseek(fid_temp, 0, SEEK_SET);
            lseek(fid_values, 0, SEEK_SET);
            while (true)
            {
                auto len = read(fid_temp, buf, sizeof(buf)-1);
                if (len==0)
                    break; // end of temporary values file
                if (len<0)
                {
                    problems |= (1<<1);
                    break;
                }
                write(fid_values, buf, sizeof(buf)-1);
                fpos_values += sizeof(buf)-1;
            }
            ftruncate(fid_values, fpos_temp);
            ::close(fid_temp);
            unlink((db_prefix+name+compact_fname).c_str());
            compacts++;
            spaces=0;
        }
        else
            problems |= (1<<3);
        if (trace_level & (1 << ETrace::FILEIO))
            applog.info("compacted: {:}, problems: {:}, errno: {:}", db_prefix+name+compact_fname, problems, errno);
        return s;
    }
    std::string evict()
    {
        if (keys.size() == max_cache)
        {
            auto key = keys.back().text;
            xref.erase(xref.find(key));
            keys.pop_back();
            values.pop_back(); // Eviction
            if (trace_level & (1 << ETrace::CACHE))
                applog.info("evict key {:}", key);
            evictions++;
            return "Eviction of key " + key;
        }
        return "Evition not required";
    }

    // Tried to use seastar::coroutine::experimental::generator<Index>
    Generator<Index> index_revolver(bool locked, int &remove)
    {
        off_t fpos = 0;
        int count = 0;
        while (true)
        {
            char buf[key_len];
            {
                Lock l(lock_cache, locked);
                lseek(fid_keys, fpos, SEEK_SET);
                auto len = read(fid_keys, buf, sizeof(buf)-1);
                if (len==0)
                {
                    if (trace_level & (1 << ETrace::FILEIO))
                        applog.info("Reached the end of {}, count {}", db_prefix+name+keys_fname, count);
                    co_yield Index{.index = -count, .vfpos = index_end_marker, .vsz = 0};
                    fpos = 0;
                    count = 0;
                    continue;
                }
                if (len<0)
                {
                    if (trace_level & (1 << ETrace::FILEIO))
                        applog.info("Error reading {}, count {}, errno {}", db_prefix+name+keys_fname, count, errno);
                    break;
                }
            }
            count++;
            char * sp = strchr(buf,',');
            *sp++=0;
            Index ind{.index=int(fpos/(sizeof(buf)-1))};
            strncpy(ind.key, buf, sizeof(ind.key));
            sscanf(sp, "%jd,%zu", &ind.vfpos, &ind.vsz);
            if (trace_level & (1 << ETrace::FILEIO))
                applog.info("{} [{}, {}, {}, {}] of index {}", ind.vfpos==index_deleted_marker ? "Skipping deleted" : "Yielding", ind.index, ind.vfpos, ind.key, ind.vsz, name+keys_fname);
            if (ind.vfpos!=index_deleted_marker)
                co_yield ind;
            fpos += sizeof(buf)-1;
        }
    }
    int delete_index = false;
    Generator<Index> co_index_locked = index_revolver(false, delete_index);
    Generator<Index> co_index_unlocked = index_revolver(true, delete_index);

    // The challenge is to get the list of keys while inserts and deletes are pounding.
    // Poorer performance than scanning the file top-down, but allows concurrent access while the list is built.
    std::string list(bool locked)
    {
        std::set<std::string> keys;
        int count = 0;
        int max_count = std::numeric_limits<int>::max();
        while (true)
        {
            auto [index, key, vfpos, vsz] = locked ? co_index_locked() : co_index_unlocked();
            if (vfpos!=index_end_marker) // reached the end, must roll over
                keys.insert(key);
            else
                if ((max_count = -index) == 0)
                    break;
            if (++count > max_count)
                break;
        }
        entries = keys.size();
        std::string s;
        for (auto& key : keys)
            s += key + "\n";
        if (trace_level & (1 << ETrace::SERVER))
            applog.info("list: {:}", s);
        return s;
    }
    auto get(const std::string &key, std::string &value, bool reentered=false, bool remove=false)
    {
        Lock l(lock_cache, reentered);
        auto x = xref.begin();
        int problems = 0;
        char buf[key_len];
        off_t fpos_values = index_end_marker;
        size_t sz;
        if (key=="")
        {
            if (remove)
                purge(false);
            else
            {
                for (auto &x : xref)
                    value += x.first + "=" + x.second->it_value->text + "\n";
                if (trace_level & (1 << ETrace::CACHE))
                    applog.info("get: (cache) {}", value);
            }
        }
        else if ((x = xref.find(key)) != xref.end())
        {
            value = x->second->it_value->text;
            if (remove)
            {
                // mark index entry as deleted.
                // The revolver coroutine should come to clean-it up.
                off_t fpos_keys = x->second->fpos;
                lseek(fid_keys, fpos_keys, SEEK_SET);
                auto len = read(fid_keys, buf, sizeof(buf)-1);
                if (len==sizeof(buf)-1)
                {
                    char * sp = strchr(buf,',');
                    *sp++=0;
                    len = sp-buf-1;
                    size_t sz;
                    sscanf(sp, "%jd,%zu", &fpos_values, &sz);
                    lseek(fid_keys, fpos_keys, SEEK_SET);
                    fpos_values = index_deleted_marker;
                    sprintf(buf, "%s,%jd,%zu,", key.c_str(), fpos_values, sz);
                    len = strlen(buf);
                    if (write(fid_keys, buf, len) != (ssize_t)len)
                        problems |= (1<<0); // Cannot write to keys file
                    len = sizeof(buf)-1-len-1;
                    sprintf(buf, "%*c\n", int(len), ' ');
                    len++;
                    if (write(fid_keys, buf, len) != (ssize_t)len)
                        problems |= (1<<1); // Cannot write to keys file
                }
                values.erase(x->second->it_value);
                keys.erase(x->second);
                xref.erase(x);
                entries--;
            }
            else
                gets++;
            if (trace_level & (1 << ETrace::CACHE))
                applog.info("{:} {:} {:} problems {}", remove ? "remove" : "get: ", key=="" ? "<ALL>" : key, value, problems);
        }
        else
        {
            off_t fpos_keys = lseek(fid_keys, 0, SEEK_SET);
            while (true)
            {
                auto len = read(fid_keys, buf, sizeof(buf)-1);
                if (len==0)
                    break; // end of index file
                if (len != sizeof(buf)-1)
                {
                    problems |= (1<<2); // didn't read enough
                    break;
                }
                char * sp = strchr(buf,',');
                *sp++=0;
                if (!strcmp(buf,key.c_str()))
                {
                    sscanf(sp, "%jd,%zu", &fpos_values, &sz);
                    if (fpos_values != index_deleted_marker)
                        break; // key found in index file
                }
                fpos_keys = lseek(fid_keys, 0, SEEK_CUR);
            }
            if (fpos_values!=index_end_marker) // Load key from index file
            {
                if (remove)
                    if (trace_level & ((1 << ETrace::FILEIO) | (1 << ETrace::CACHE)))
                        applog.info("get: (remove) {:} {:} {:}", key, value, problems);
                lseek(fid_values, fpos_values, SEEK_SET);
                value.resize(sz);
                if (read(fid_values, (char*)value.c_str(), sz) != (ssize_t)sz)
                    problems |= (1<<3); // Cannot read from values file
                else
                {
                    evict();
                    values.push_front({value, fpos_values, sz});
                    keys.push_front({buf, fpos_keys, values.begin()});
                    x = xref.insert_or_assign(key, keys.begin()).first;
                    if (trace_level & ((1 << ETrace::FILEIO) | (1 << ETrace::CACHE)))
                        applog.info("get: (cachefill) {:} {:} {:}", key, value, problems);
                }
            }
            else
            {
                x = xref.end();
                if (trace_level & ((1 << ETrace::FILEIO) | (1 << ETrace::CACHE)))
                    applog.info("get: key {:} not found", key);
            }
        }
        struct RV{bool found; decltype(x) it; };
        return RV{x != xref.end(), x};
    }
    bool set(const std::string &key, const std::string &val, bool update)
    {
        bool rv = true;
        Lock l(lock_cache, false);
        std::string old_val;
        auto [found, x] = get(key, old_val, true);
        int problems = 0;
        std::string branch = "Skip";
        if (!update || val != old_val) // not found, or found different for update
        {
            bool overwrite = false;
            char buf[key_len];
            if (!found) // key not in cache and not in file
            {
                if (update)
                    rv = false;
                else // insert
                {
                    evict();
                    off_t fpos_keys = lseek(fid_keys, 0, SEEK_END);
                    off_t fpos_values = index_end_marker;
                    //size_t sz;
                    fpos_values = lseek(fid_values, 0, SEEK_END);
                    values.push_front({val, fpos_values, 0});
                    keys.push_front({key, fpos_keys, values.begin()});
                    x = xref.insert_or_assign(key, keys.begin()).first;
                    branch = "Add";
                    entries++;
                    inserts++;
                }
            }
            else
            {
                branch = "Overwrite";
                overwrite = true;
                if (update)
                    updates++;
                else
                    rv = false;
            }
            if (rv && x != xref.end())
            {
                Value& oldv = *x->second->it_value;
                if (overwrite && val == oldv.text)
                    branch = "ignore";
                else
                {
                    auto sz = oldv.sz;
                    bool reuse_value_space = val.length() <= sz;
                    off_t fpos_values = 0;
                    if (reuse_value_space)
                    {
                        spaces += sz-val.length();
                        fpos_values = oldv.fpos;
                    }
                    else
                        spaces += sz;
                    sz = val.length();
                    fpos_values = lseek(fid_values, fpos_values, reuse_value_space ? SEEK_SET : SEEK_END);
                    if (write(fid_values, val.c_str(), val.length()) != (ssize_t)val.length())
                        problems |= (1<<0); // Cannot write to values file
                    values.erase(x->second->it_value);
                    values.push_front({val, fpos_values, sz});
                    sprintf(buf, "%s,%jd,%zu,", key.c_str(), fpos_values, sz);
                    auto len = strlen(buf);
                    auto fpos_keys = x->second->fpos;
                    lseek(fid_keys, x->second->fpos, SEEK_SET);
                    if (write(fid_keys, buf, len) != (ssize_t)len)
                        problems |= (1<<1); // Cannot write to keys file
                    len = sizeof(buf)-1-len-1;
                    sprintf(buf, "%*c\n", int(len), ' ');
                    len++;
                    if (write(fid_keys, buf, len) != (ssize_t)len)
                        problems |= (1<<2); // Cannot write to keys file
                    keys.erase(x->second);
                    keys.push_front({key, fpos_keys, values.begin()});
                    x->second = keys.begin();
                }
            }
        }
        if (trace_level & ((1 << ETrace::FILEIO) | (1 << ETrace::CACHE)))
            applog.info("set: {:} {:} {:} problems: {:}, result: {:}", branch, key, val, problems, rv);
        return rv;
    }
    operator sstring() const
    {
        return sstring(VERSION) + "\n" +
            "table:" + name + "\n" +
            Stats::operator::sstring() +
        format(
            "index entries:{}\n"
            "cache_entries:{}\n"
            "lock_cache:{}\n"
            "lock_fio:{}\n"
            "readers:{}\n"
            "value spaces:{}\n",
            entries,
            xref.size(),
            lock_cache,
            lock_fio,
            readers,
            spaces
        );
    }
    static auto selftest(int level)
    {
        bool ok = true;
        std::string s;
        s += format("Starting self test {}\n", level);
        auto table_name = std::string(Table::master_table_name);
        s += "Dropping & using {}\n" + table_name;
        auto it_table = tables.find(table_name);
        if (it_table == tables.end())
        {
            it_table = tables.emplace(table_name, new Table(table_name)).first;
            auto &table = *it_table->second;
            s += "Using table " + table_name + ".\n";
            s += table.purge(true);
            s += table.close(true);
            Table::tables.erase(it_table);
            s += "Dropping table " + table_name + ".\n";
            it_table = tables.emplace(table_name, new Table(table_name)).first;
            s += "(Re)create table " + table_name + ".\n";
        }
        auto &table = *it_table->second;
        std::string keys[] = {"aaa", "bbb", "ccc", "null"};
        std::string values0[] = {"test_aaa\n", "test_bbb\n", "test_ccc\n", ""};
        for (int i=0; auto key : keys)
            if (auto value = values0[i++]; !table.set(key, value, false))  // insert
            {
                s += format("Failed to insert {}={}\n", key, value);
                ok = false;
            }
        for (int i=0; auto key : keys)
        {
            std::string value;
            if (!table.get(key, value).found)  // read
            {
                s += format("Failed to retrieve {}\n", key);
                ok = false;
            }
            if (value!=values0[i])
            {
                s += format("Unexpected value for key {}={} <> {}\n", key, value, values0[i]);
                ok = false;
            }
            i++;
        }
        for (int i=0; auto key : keys)
            if (auto value = values0[i++]; table.set(key, value, false))  // overinsert
            {
                s += format("Failed to prevent overinsertion on pre-existing key {}={}\n", key, value);
                ok = false;
            }
        std::string values1[] = {"test_AAAA\n", "test_BBB\n", "test_CC\n", "text_NotNull\n"};
        for (int i=0; auto key : keys)
            if (auto value = values1[i++]; !table.set(key, value, true))  // update
            {
                s += format("Failed to update existing key {}={}\n", key, value);
                ok = false;
            }
        if (auto key = "ddd", value="text_ddd"; table.set(key, value, true))  // update inexistent key
        {
            s += format("Failed to prevent updating of inexistent key {}={}\n", key, value);
            ok = false;
        }
        for (int i=0; auto key : keys)
        {
            if (std::string value; !table.get(key, value).found || value!=values1[i])
            {
                s += format("Unexpected value for key {}={}, expected {}\n", key, value, values1[i]);
                ok = false;
            }
            i++;
        }
        if (table.spaces==0)
            s += format("Table {} expected to have {} internal fragmentation\n", table.name, "non-zero");

        s += table.compact(); // Compacting must reorder the values file.

        for (int i=0; auto key : keys)
        {
            if (std::string value; !table.get(key, value, false, true).found) // delete
            {
                s += format("Delete failed for key {}={}, expected {}\n", key, value, values1[i]);
                ok = false;
            }
            i++;
            break; // do just one delete
        }

        s += table.compact(); // Compacting remove all index entries marked as "deleted" and reorders the value file.

        if (auto count = sizeof(keys)/sizeof(keys[0]) - 1; table.entries != count)
        {
            s += format("Unexpected number of keys in table {}: {}, expected {}\n", table.name, table.entries, count);
            ok = false;
        }

        s += table.list(true); // revolve though index keys once

        s += format("Self test {} {}\n", level, ok ? "passed" : "failed");
        return s;
    }
    bool get_file(std::string &val, bool keys)
    {
        auto fid = keys ? fid_keys : fid_values;
        if (fid<=0)
            return false;
        Lock l(lock_fio);
        auto sz = lseek(fid, 0, SEEK_END);
        if (sz<0)
            return false;
        val.resize(sz);
        lseek(fid, 0, SEEK_SET);
        return ::read(fid, (char*)val.c_str(), sz) == sz;
    }
private:
    constexpr static size_t key_len = 256+1 +20+10+1+1; // key,int64,int32\n0
    constexpr static int index_end_marker = -1;
    constexpr static int index_deleted_marker = -2;

    std::atomic_bool lock_fio = false;   // file lock: mutually exclussive readers and writers
    std::atomic_bool lock_cache = false; // cache lock: shared readers, exclusive writers
    std::atomic_int  readers = 0;        // count of concurrent readers

    int fid_keys{0};
    int fid_values{0};
    size_t entries{0};
    size_t spaces{0};
    std::deque<Value> values;
    std::deque<Key> keys;
    std::map<std::string, std::deque<Key>::iterator> xref;
};
std::map<std::string, Table*> Table::tables;
std::atomic_bool tables_lock = false;

class DefaultHandle : public httpd::handler_base {
public:
    bool plain_text;
    DefaultHandle(bool plain_text) : plain_text(plain_text) {}
    auto build_help(sstring url, sstring host, sstring table_name)
    {
        sstring s;
        s += plain_text ?
            format("{:}.{:}\n", VERSION) +
            format("Global commands\n")
        :
            sstring("<h1>") + VERSION + "</h1>\n" +
            "<html>\n<head>\n<style>\n"
            "body {background-color: white;}\n"
            "h1   {color: black;}\n"
            "span {color: black; min-width: 25%; display: inline-block;}\n"
            "p    {color: red;background-color: FloralWhite;}\n"
            "a {text-decoration: none;}"
            "</style>\n</head>\n<body>\n" +
            sstring("<h1>Global commands:</h1>\n<p>");
        struct {const char*label, *str; } args_global[] =
        {
            {"QUIT",                "quit"},
            {"trace level all ON",  "?trace_level=1111"},
            {"trace level all OFF", "?trace_level=0000"},
            {"list tables in use",  "?used"},
        };
        for (auto arg : args_global)
            s += plain_text ?
            format("{:}: {:}/{:}\n", arg.label, host, arg.str) :
            sstring("<span>") + arg.label + ":</span> <a href=\"" + host + "/" + arg.str + "\">" + host + "/" + arg.str + "</a><br/>\n";
        s += plain_text ?
            format("Commands for table: {:}\n", table_name) :
            sstring("</p>\n<h1>Commands for table: " + table_name + "&nbsp</h1>\n"
            "For any of the following commands to work, a table must be created, or opened if it exists, with the <strong><a href=\"" + host + "/?use\">?use</a></strong> REST command.\n"
            "<p>");
        struct {const char*label, *str; } args_per_table[] =
        {
            {"use/create table",        "?use"},
            {"change to table Master",  "/"},
            {"change to table People",  "People/"},
            {"change to table Places",  "Places/"},
            {"stats",                   "?stats"},
            {"config max_cache",        "?max_cache=2"},
            {"config max_cache",        "?max_cache=1000"},
            {"purge cache & files",     "?purge"},
            {"drop table",              "?drop"},
            {"compact data files",      "?compact"},
            {"invalidate cache",        "?invalidate"},
            {"list cached keys/vals",   "?key="},
            {"list all keys",           "?list"},
            {"row next",                "?rownext"},
            {"row next",                "?rownext"},
            {"insert aaa=text_aaa",     "?op=insert&key=aaa&value=text_aaa%0A"},
            {"insert bbb=text_bbb",     "?op=insert&key=bbb&value=text_bbb%0A"},
            {"insert ccc=text_ccc",     "?op=insert&key=ccc&value=text_ccc%0A"},
            {"insert null=NULL",        "?op=insert&key=null&value="},
            {"query aaa",               "?key=aaa"},
            {"query bbb",               "?key=bbb"},
            {"query ccc",               "?key=ccc"},
            {"query null",              "?key=null"},
            {"update aaa=text_aaaa",    "?op=update&key=aaa&value=text_aaaa%0A"},
            {"update bbb=text_BBB",     "?op=update&key=bbb&value=text_BBB%0A"},
            {"update ccc=text_cc",      "?op=update&key=ccc&value=text_cc%0A"},
            {"update null=text_nonnull","?op=update&key=null&value=text_nonnull%0A"},
            {"delete aaa",              "?op=delete&key=aaa"},
            {"delete bbb",              "?op=delete&key=bbb"},
            {"delete ccc",              "?op=delete&key=ccc"},
            {"delete null",             "?op=delete&key=null"},
            {"view keys file",          "?file=keys"},
            {"view values file",        "?file=values"},
        };
        for (int i=0; auto arg : args_per_table)
        {
            i++;
            if ((i==2 && table_name==Table::master_table_name) ||
                (i>2 && i<5 && table_name!=Table::master_table_name))
                continue;
            auto surl = url;
            if (i==2 && table_name!=Table::master_table_name)
            {
                auto sl = strrchr(url.c_str(),'/');
                auto tmp = url.substr(0, sl-url.c_str());
                sl = strrchr(tmp.c_str(),'/');
                surl = tmp.substr(0, sl-tmp.c_str());
            }
            s += plain_text ?
            format("{:}: {:}/{:}\n", arg.label, surl, arg.str) :
            sstring("<span>") + arg.label + ":</span> <a href=\"" + surl + arg.str + "\">" + arg.str + "</a><br/>\n";
        }
        struct {const char*label, *str; } args_tests[] =
        {
            {"simple insert/update/delete",     "?selftest=0"},
            {"poop insert/update/delete",       "?selftest=1"},
        };
        s += plain_text ?
            format("Self tests:\n") :
            sstring("</p>\n<h1>Self tests:""&nbsp</h1>\n");
        for (auto arg : args_tests)
            s += plain_text ?
            format("{:}: {:}/{:}\n", arg.label, host, arg.str) :
            sstring("<span>") + arg.label + ":</span> <a href=\"" + host + "/" + arg.str + "\">" + arg.str + "</a><br/>\n";
        s += plain_text ?
            "" :
            "</p></span>\n</body>";
        return s;
    }
    virtual future<std::unique_ptr<http::reply> > handle(const sstring& path,
            std::unique_ptr<http::request> req, std::unique_ptr<http::reply> rep)
    {
        auto host = req->get_protocol_name() + "://" + req->get_header("Host");
        auto url = req->get_url();
        auto table_name = url.substr(host.length()+1);
        auto fsl = (int)table_name.find('/', 0);
        if (fsl>0)
            table_name = table_name.substr(0,fsl);
        else
            table_name = Table::master_table_name;
        sstring s = "";
        bool plain = plain_text;
        auto it_table = Table::tables.end();
        if (req->query_parameters.size()==0) // HELP
            s += build_help(url, host, table_name);
        else if (req->query_parameters.contains("trace_level"))
        {
            auto level = req->query_parameters.at("trace_level");
            int old_trace_level = trace_level;
            int new_trace_level = 0;
            std::string old_tl = "";
            std::string new_tl = "";
            for (size_t i=0; i<level.length(); i++)
            {
                if (i>0)
                {
                    old_tl += ",";
                    new_tl += ",";
                }
                if (old_trace_level & (1<<i))
                    old_tl += "*";
                old_tl += available_trace_levels[i];
                if (level[i]=='1')
                    new_trace_level |= (1<<i);
                else if (level[i]=='0')
                    new_trace_level &= ~(1<<i);
                if (new_trace_level & (1<<i))
                    new_tl += "*";
                new_tl += available_trace_levels[i];
            }
            s = format("Changing trace_level from {} to {}", old_tl, new_tl);
            trace_level = new_trace_level;
            plain = true;
        }
        else if (req->query_parameters.contains("use"))
        {
            Lock l(tables_lock);
            it_table = Table::tables.find(table_name);
            if (it_table == Table::tables.end())
            {
                auto & table = *(Table::tables.emplace(table_name, new Table(table_name)).first->second);
                s += "Created table " + table_name + ".\n";
            }
        }
        else if (req->query_parameters.contains("used"))
        {
            Lock l(tables_lock);
            for (auto t : Table::tables) s += t.first + "\n";
        }
        else if (req->query_parameters.contains("selftest"))
        {
            auto level = req->query_parameters.at("selftest");
            int l;
            sscanf(level.c_str(),"%d", &l);
            std::string result = Table::selftest(l);
            std::string::size_type n = 0;
            const std::string rwhat = "\n";
            const std::string rwith = "<br/>\n";
            while ((n = result.find(rwhat, n)) != std::string::npos)
            {
                result.replace( n, rwhat.size(), rwith);
                n += rwith.size();
            }
            s += result;
        }
        else
        {
            {
                Lock l(tables_lock);
                it_table = Table::tables.find(table_name);
                if (it_table == Table::tables.end())
                    s += "Table " + table_name + " is not in use.\n";
            }
            std::string new_val;
            if (it_table != Table::tables.end())
            {
                auto &table = *it_table->second;
                if (req->query_parameters.contains("list"))
                    s = table.list(false);
                else if (req->query_parameters.contains("invalidate"))
                    s = table.purge(false);
                else if (req->query_parameters.contains("compact"))
                    s = table.compact();
                else if (req->query_parameters.contains("purge"))
                    s = table.purge(true);
                else if (req->query_parameters.contains("stats"))
                    s += sstring(s.length() > 0 ? "\n" : "") + format(
                        "{}"
                        "LockPasses:{}\n"
                        "LockCollisions:{}\n"
                        ,
                        (sstring)table,
                        Lock::passes,
                        Lock::collisions);
                else if (req->query_parameters.contains("drop"))
                {
                    s += table.purge(true);
                    s += table.close(true);
                    Table::tables.erase(it_table);
                }
                else if (req->query_parameters.contains("max_cache"))
                {
                    auto val = req->query_parameters.at("max_cache");
                    int old_max_cache = table.max_cache;
                    int new_max_cache;
                    sscanf(val.c_str(), "%d", &new_max_cache);
                    if (new_max_cache>=0)
                    {
                        table.purge(false);
                        table.max_cache = new_max_cache;
                    }
                    s += sstring(s.length() > 0 ? "\n" : "") + format(
                        "Changing max_cache from {:} to {:}", old_max_cache, new_max_cache);
                }
                else if (req->query_parameters.contains("file"))
                {
                    auto val = req->query_parameters.at("file");
                    std::string text;
                    if (table.get_file(text, val=="keys"))
                        s += text;
                    else
                        s += format("Error reading {} file", val);
                }
                else if (req->query_parameters.contains("rownext"))
                {
                    auto [index, key, fpos, sz] = table.co_index_locked();
                    s = format("test index {:}, key:{:}, valpos:{:}, valsz:{:}", index, key, fpos, sz);
                }
                else if (req->query_parameters.contains("op"))
                {
                    auto op = req->query_parameters.at("op");
                    auto key = req->query_parameters.contains("key") ? req->query_parameters.at("key") : "";
                    auto value = req->query_parameters.contains("value") ? req->query_parameters.at("value") : "";
                    if (op=="insert")
                        s = table.set(key, value, false) ? "ok" : "fail";
                    else if (op=="update")
                        s = table.set(key, value, true) ? "ok" : "fail";
                    else if (op=="delete")
                        s = table.get(key, new_val, false, true).found ? sstring(new_val) : format("Key {} not found", key);
                    else // any other op is a key read
                        s = table.get(key, new_val).found ? sstring(new_val) : format("Key {} not found", key);
                }
                else if (req->query_parameters.contains("key"))
                {
                    auto key = req->query_parameters.at("key");
                    s = table.get(key, new_val).found ? sstring(new_val) : format("Key {} not found", key);
                }
            }
            if (trace_level & (1 << ETrace::SERVER))
                applog.info("DB request {} on table {}, {}", Stats::requests++, table_name, s);
            plain = true;
        }
        rep->_content = s;
        rep->done(plain ? "plain" : "html");
        return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
    }
} defaultHandle(false);

void set_routes(routes& r) {
    function_handler* h4 = new function_handler([](const_req req) {
        std::string s = "Server shutting down";
        exit(0);
        if (trace_level & (1 << ETrace::SERVER))
            applog.info("{:}", s);
        return s;
    });
    r.add_default_handler(&defaultHandle);
    r.add(operation_type::GET, url("/quit"), h4);
    r.add(operation_type::GET, url("/file").remainder("path"), new directory_handler("/"));
}

int main(int ac, char** av) {
    applog.info("Db demo {:}", VERSION);
    httpd::http_server_control prometheus_server;
    prometheus::config pctx;
    app_template app;

    app.add_options()("port", bpo::value<uint16_t>()->default_value(10000), "HTTP Server port");
    app.add_options()("prometheus_port", bpo::value<uint16_t>()->default_value(9180), "Prometheus port. Set to zero in order to disable.");
    app.add_options()("prometheus_address", bpo::value<sstring>()->default_value("0.0.0.0"), "Prometheus address");
    app.add_options()("prometheus_prefix", bpo::value<sstring>()->default_value("seastar_httpd"), "Prometheus metrics prefix");

    return app.run(ac, av, [&] {
        return seastar::async([&] {
            struct stop_signal {
                bool _caught = false;
                seastar::condition_variable _cond;
                void signaled() {
                    if (_caught)
                        return;
                    _caught = true;
                    _cond.broadcast();
                }
                stop_signal() {
                    seastar::engine().handle_signal(SIGINT, [this] { signaled(); });
                    seastar::engine().handle_signal(SIGTERM, [this] { signaled(); });
                }
                ~stop_signal() {
                    // There's no way to unregister a handler yet, so register a no-op handler instead.
                    seastar::engine().handle_signal(SIGINT, [] {});
                    seastar::engine().handle_signal(SIGTERM, [] {});
                }
                seastar::future<> wait() { return _cond.wait([this] { return _caught; }); }
                bool stopping() const { return _caught; }
            } stop_signal;
            auto&& config = app.configuration();
            httpd::http_server_control prometheus_server;
            bool prometheus_started = false;

            auto stop_prometheus = defer([&] () noexcept {
                if (prometheus_started) {
                    std::cout << "Stoppping Prometheus server" << std::endl;  // This can throw, but won't.
                    prometheus_server.stop().get();
                }
            });

            uint16_t pport = config["prometheus_port"].as<uint16_t>();
            if (pport) {
                prometheus::config pctx;
                net::inet_address prom_addr(config["prometheus_address"].as<sstring>());

                pctx.metric_help = "seastar::httpd server statistics";
                pctx.prefix = config["prometheus_prefix"].as<sstring>();

                std::cout << "starting prometheus API server" << std::endl;
                prometheus_server.start("prometheus").get();

                prometheus::start(prometheus_server, pctx).get();

                prometheus_started = true;

                prometheus_server.listen(socket_address{prom_addr, pport}).handle_exception([prom_addr, pport] (auto ep) {
                    std::cerr << seastar::format("Could not start Prometheus API server on {}:{}: {}\n", prom_addr, pport, ep);
                    return make_exception_future<>(ep);
                }).get();

            }

            uint16_t port = config["port"].as<uint16_t>();
            auto server = new http_server_control();
            auto rb = make_shared<api_registry_builder>("apps/httpd/");
            server->start().get();

            auto stop_server = defer([&] () noexcept {
                std::cout << "Stoppping HTTP server" << std::endl; // This can throw, but won't.
                server->stop().get();
            });

            server->set_routes(set_routes).get();
            server->set_routes([rb](routes& r){rb->set_api_doc(r);}).get();
            server->set_routes([rb](routes& r) {rb->register_function(r, "demo", "hello world application");}).get();
            server->listen(port).get();

            std::cout << "Seastar HTTP server listening on port " << port << " ...\n";

            stop_signal.wait().get();
            return 0;
        });
    });
}
