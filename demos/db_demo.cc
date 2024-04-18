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
 * - Cache is currently WRITETHROUGH.
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
#include <seastar/core/sleep.hh>

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wstringop-truncation"

namespace bpo = boost::program_options;

using namespace seastar;
using namespace httpd;

const char*VERSION="Demo Database Server 0.7";
const char* available_trace_levels[] = {"LOCKS", "FILEIO", "CACHE", "SERVER", "TEST"};
enum ETrace { LOCKS, FILEIO, CACHE, SERVER, TEST, MAX};
int trace_level = (1 << ETrace::FILEIO) | (1 << ETrace::CACHE) | (1 << ETrace::SERVER) | (1 << ETrace::TEST);
logger applog("app");

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
        if (trace_level & (1 << ETrace::LOCKS)) applog.info("LOCKS: Locked");
    }
    ~Lock()
    {
        if (reentered)
            return;
        if (trace_level & (1 << ETrace::LOCKS)) applog.info("LOCKS: Unlocked");
        value = false;
    }
    static std::atomic_int passes;
    static std::atomic_int collisions;
};
std::atomic_int Lock::passes = 0;
std::atomic_int Lock::collisions = 0;

struct Stats
{
    static std::atomic_int requests, creates, drops, self_tests;
    std::atomic_int inserts{0}, deletes{0}, updates{0}, gets{0}, purges{0}, invalidates{0}, evictions{0}, compacts{0};
    static std::string get_static_profiles()
    {
        char buf[0x1000];
        sprintf(buf,
            "requests: %d\n"
            "creates: %d\n"
            "drops: %d\n"
            "self_tests: %d\n"
            "LockPasses: %d\n"
            "LockCollisions: %d\n"
            ,
            requests.load(),
            creates.load(),
            drops.load(),
            self_tests.load(),
            Lock::passes.load(),
            Lock::collisions.load()
        );
        return buf;
    }
    operator std::string() const
    {
        char buf[0x1000];
        sprintf(buf,
            "inserts: %d\n"
            "deletes: %d\n"
            "updates: %d\n"
            "gets: %d\n"
            "purges: %d\n"
            "invalidates: %d\n"
            "evictions: %d\n"
            "compacts: %d\n"
            ,
            inserts.load(),
            deletes.load(),
            updates.load(),
            gets.load(),
            purges.load(),
            invalidates.load(),
            evictions.load(),
            compacts.load()
        );
        return get_static_profiles() + buf;
    }
};
std::atomic_int Stats::requests = 0;
std::atomic_int Stats::creates = 0;
std::atomic_int Stats::drops = 0;
std::atomic_int Stats::self_tests = 0;

struct Table : Stats {
    std::string name;
    size_t max_cache {2};
    static bool mode_async;

    constexpr static auto db_prefix = "db_";
    constexpr static auto keys_fname = "_keys.txt";
    constexpr static auto values_fname = "_values.txt";
    constexpr static auto compact_fname = "_compact.txt";
    constexpr static auto console_fname = "_console.txt";
    constexpr static auto master_table_name = "Master";

    static std::map<std::string, Table*> tables;

    struct Index
    {
        int index;
        char key[256];
        off_t vfpos;
        ssize_t vsz;
        constexpr static size_t entry_sz = 256 + 44; // 300 = key,int64,int32,extra\n0
        constexpr static off_t end_marker = -1;
        constexpr static off_t deleted_marker = -2;
        void access_value(int fid_values, std::string *value, std::string &table_name, bool write)
        {
            sstring msg;
            if (write)
            {
                if (value!=nullptr && value->length()>0)
                {
                    vsz = value->length();
                    vfpos = lseek(fid_values, 0 , SEEK_END);
                    if (::write(fid_values, value->c_str(), vsz) != vsz)
                        msg+=format(", Error {} writing values file fid:{}", errno, fid_values);
                    if (trace_level & (1 << ETrace::FILEIO))
                        applog.info("FILEIO: Value write {}{}", *value, msg);
                }
            }
            else
            {
                if (value!=nullptr && vfpos>=0 && vsz>0)
                {
                    lseek(fid_values, vfpos, SEEK_SET);
                    value->resize(vsz, 0);
                    if (::read(fid_values, (char*)value->c_str(), vsz) != vsz)
                        msg+=format(", Error {} reading values filefid:{}", errno, fid_values);
                    if (trace_level & (1 << ETrace::FILEIO))
                        applog.info("FILEIO: Value write {}{}", *value, msg);
                }
            }
        }
        Index& access(int fid_keys, int fid_values, std::string *value, std::string &table_name, bool write)
        {
            auto fname = db_prefix+table_name+keys_fname;
            char buf[entry_sz+1];
            ssize_t len = 0;
            sstring msg;
            if (write)
            {
                access_value(fid_values, value, table_name, write); // Read value from values file
                auto fpos_keys = lseek(fid_keys, index * entry_sz, SEEK_SET);
                sprintf(buf, "%s,%jd,%zu,", key, vfpos, vsz);
                len = strlen(buf);
                sprintf(buf+len, "%*c\n", int(Index::entry_sz-1-len), ' ');
                lseek(fid_keys, fpos_keys, SEEK_SET);
                if (::write(fid_keys, buf, Index::entry_sz) != Index::entry_sz)
                    msg+=format(", Error {} writing keys file", errno);
                if (trace_level & (1 << ETrace::FILEIO))
                    applog.info("FILEIO: Index write {}[{}] {}{} {}{}", fname, index, key, value==nullptr ? "" : ("=" + *value), vfpos == deleted_marker ? ", deleted" : "", msg);
            }
            else
            {
                auto fpos_keys = lseek(fid_keys, 0, SEEK_CUR);
                auto fpos_next = fpos_keys;
                index = fpos_keys / entry_sz;
                int deleted_keys = 0;
                vfpos = end_marker;
                len = ::read(fid_keys, buf, Index::entry_sz);
                if (len<0)
                    msg+=format(", Error1 {} reading keys file", errno);
                else if (len==0)
                    msg+=format(", EIF at {}", fpos_keys);
                else if (len!=Index::entry_sz)
                    msg+=format(", Could not read a full index line, errno:{}", errno);
                else while (len==Index::entry_sz)
                {
                    char * sp = strchr(buf,',');
                    strncpy(key, buf, sp-buf);
                    key[sp-buf]=0;
                    sscanf(sp+1, "%jd,%zu", &vfpos, &vsz);
                    if (vfpos!=Index::deleted_marker)
                        break;
                    vfpos = end_marker;
                    deleted_keys++;
                    char buf_next[Index::entry_sz+1];
                    fpos_next += Index::entry_sz;
                    auto len = ::read(fid_keys, buf_next, Index::entry_sz);
                    if (len<0)
                    {
                        msg+=format(", Error2 {} reading keys file", errno);
                        break;
                    }
                    if (len==0)  // end of index file
                    {
                        msg+=format(", EIF at {}", fpos_keys);
                        ftruncate(fid_keys, fpos_keys);
                        break;
                    }
                    lseek(fid_keys, fpos_keys, SEEK_SET);
                    ::write(fid_keys, buf_next, Index::entry_sz);       // Shift the next index entry up by one.
                    lseek(fid_keys, fpos_next, SEEK_SET);
                    ::write(fid_keys, buf, Index::entry_sz);            // Swap curent and next index entries.
                    memcpy(buf, buf_next, Index::entry_sz);
                }
                access_value(fid_values, value, table_name, write); // Write value to file
                if (deleted_keys>0)
                    msg+=format(" compacted {} index entries", deleted_keys);
                if (trace_level & (1 << ETrace::FILEIO))
                    applog.info("FILEIO: index read {}[{}] {}{}{}", fname, index, key, value==nullptr ? "" : ("=" + *value), msg);
            }
            return *this;
        }
    };

    Table(Table const &) = delete;
    void operator=(Table const &) = delete;
    explicit Table(std::string name) : name(name)
    {
        fid_keys = open((db_prefix+name+keys_fname).c_str(), O_CREAT | O_RDWR, S_IREAD|S_IWRITE|S_IRGRP|S_IROTH);
        if (trace_level & (1 << ETrace::FILEIO))
            applog.info("FILEIO: Opened {} returned {}, {}", db_prefix+name+keys_fname, fid_keys, errno);
        fid_values = open((db_prefix+name+values_fname).c_str(), O_CREAT | O_RDWR, S_IREAD|S_IWRITE|S_IRGRP|S_IROTH);
        if (trace_level & (1 << ETrace::FILEIO))
            applog.info("FILEIO: Opened {} returned {}", name+keys_fname, fid_values, errno);
        creates++;
    }
    std::string close(bool remove_files)
    {
        if (fid_keys>0)
            ::close(fid_keys);
        if (fid_values>0)
            ::close(fid_values);
        if (!remove_files)
            return "Closed files " + (db_prefix+name+keys_fname) + " and " + (db_prefix+name+values_fname);
        drops++;
        unlink((db_prefix+name+keys_fname).c_str());
        unlink((db_prefix+name+values_fname).c_str());
        unlink((db_prefix+name+compact_fname).c_str());
        return "Removed files " + (db_prefix+name+keys_fname) + " and " + (db_prefix+name+values_fname);
    }
    std::string purge(bool files)
    {
        Lock l(lock_cache);
        values.clear();
        keys.clear();
        xref.clear();
        std::string s = "Purged cache";
        if (files)
        {
            s += " and files";
            entries = 0;
            spaces = 0;
            ftruncate(fid_values, 0);
            ftruncate(fid_keys, 0);
            purges++;
        }
        invalidates++;
        if (trace_level & ((1 << ETrace::CACHE) | (1 << ETrace::FILEIO)))
            applog.info("CACHE|FILEIO: purge cache{}", files ? " and files" : "");
        return s;
    }
    std::string compact()
    {
        Lock l(lock_cache);
        std::string s = purge(false);
        sstring msg;
        auto fid_temp = open((db_prefix+name+compact_fname).c_str(), O_CREAT | O_TRUNC | O_RDWR /*| O_TMPFILE*/, S_IREAD|S_IWRITE|S_IRGRP|S_IROTH);
        if (fid_temp>0)
        {
            lseek(fid_keys, 0, SEEK_SET);
            int deleted = 0;
            int index = 0;
            while (true)
            {
                Index entry;
                std::string value;
                entry.access(fid_keys, fid_values, &value, name, false); // read entry
                if (entry.vfpos == Index::end_marker)
                    break;
                entry.index = index++;
                entry.access(fid_keys, fid_temp, &value, name, true); // write entry
            }
            lseek(fid_temp, 0, SEEK_SET);
            lseek(fid_values, 0, SEEK_SET);
            char buf[0x1000];
            while (auto len = ::read(fid_temp, buf, Index::entry_sz))
            {
                if (len==0)
                    break; // end of temp values file
                if (len<0)
                {
                    msg+=format(", Error {} reading temp values file", errno);
                    break;
                }
                ::write(fid_values, buf, len);
            }
            ftruncate(fid_values, lseek(fid_values, 0, SEEK_CUR));
            ::close(fid_temp);
            unlink((db_prefix+name+compact_fname).c_str());
            compacts++;
            spaces=0;
            s = format("Compacted index to {} entries. ", index) + s;
        }
        else
            msg+=format(", Error {} creating temp values file", errno);
        if (trace_level & (1 << ETrace::FILEIO))
            applog.info("FILEIO: {}{}{}", s, db_prefix+name+compact_fname, msg);
        return s;
    }

    int delete_index = false;
    seastar::coroutine::experimental::generator<Index> co_index_locked = index_revolver(false, delete_index);
    seastar::coroutine::experimental::generator<Index> co_index_unlocked = index_revolver(true, delete_index);

    // The challenge is to get the list of keys while inserts and deletes are pounding.
    // Poorer performance than scanning the file top-down, but allows concurrent access while the list is built.
    seastar::future<std::string> list(bool locked)
    {
        std::set<std::string> keys;
        int count = 0;
        int max_count = std::numeric_limits<int>::max();
        while (true)
        {
            {
                auto &generator = locked ? co_index_locked : co_index_unlocked;
                auto val = co_await generator();
                auto [index, key, vfpos, vsz] = val.value();
                if (vfpos != Index::end_marker)
                    keys.insert(key);
                else // reached the end, must roll over
                    if ((max_count = -index) == 0)
                        break;
            }
            if (++count > max_count)
                break;
        }
        entries = keys.size();
        std::string s;
        for (auto& key : keys)
            s += key + "\n";
        if (trace_level & (1 << ETrace::FILEIO))
            applog.info("FILEIO: list: {}", s);
        co_return s;
    }
    auto get(const std::string &key, std::string &value, bool remove = false, bool reentered = false)
    {
        Lock l(lock_cache, reentered);
        auto x = xref.begin();
        bool found = true;
        if (key=="")
        {
            if (remove) // clear the cache
                purge(false);
            else // list cache content
            {
                for (auto &x : xref)
                    value += x.first + "=" + x.second->it_value->text + "\n";
                if (trace_level & (1 << ETrace::CACHE))
                    applog.info("CACHE: get (cache) {}", value);
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
                Index entry;
                entry.access(fid_keys, fid_values, nullptr, name, false); // Read index entry from file ignoring value
                entry.vfpos = Index::deleted_marker;
                entry.access(fid_keys, fid_values, nullptr, name, true); // Write index entry back to file ignoring value
                values.erase(x->second->it_value);
                keys.erase(x->second);
                xref.erase(x);
                entries--;
            }
            else
                gets++;
            if (trace_level & (1 << ETrace::CACHE))
                applog.info("CACHE: get {} {} {}", remove ? "(remove)" : "", key=="" ? "<ALL>" : key, value);
        }
        else
        {
            lseek(fid_keys, 0, SEEK_SET);
            Index entry;
            while (true)
            {
                entry.access(fid_keys, fid_values, nullptr, name, false); // Read index entry from file ignoring value
                if (entry.vfpos==Index::end_marker)
                    break;
                if (key==entry.key)
                    break;
            }
            if (entry.vfpos != Index::end_marker)
            {
                if (remove)
                {
                    entry.vfpos = Index::deleted_marker;
                    entry.access(fid_keys, fid_values, nullptr, name, true); // Write index entry back to file ignoring value
                    entries--;
                    if (trace_level & ((1 << ETrace::FILEIO) | (1 << ETrace::CACHE)))
                        applog.info("CACHE|FILEIO: get (remove) {} {}", key, value);
                }
                else
                {
                    entry.access_value(fid_values, &value, name, false); // Read value from values file
                    evict();
                    off_t fpos_keys = entry.index * Index::entry_sz;
                    values.push_front({value, entry.vfpos, entry.vsz});
                    keys.push_front({key, fpos_keys, values.begin()});
                    x = xref.insert_or_assign(key, keys.begin()).first;
                    if (trace_level & ((1 << ETrace::FILEIO) | (1 << ETrace::CACHE)))
                        applog.info("CACHE|FILEIO: get (cachefill) {}={}", key, value);
                }
            }
            else
            {
                found = false;
                x = xref.end();
                if (trace_level & ((1 << ETrace::FILEIO) | (1 << ETrace::CACHE)))
                    applog.info("CACHE|FILEIO: get key {} not found", key);
            }
        }
        struct RV{bool found; decltype(x) it; };
        return RV{found, x};
    }
    bool set(const std::string &key, const std::string &value, bool update, bool reentered = false)
    {
        std::string branch;
        if (key=="null")
            if (value=="")
                branch = "trap ";

        Lock l(lock_cache, reentered);
        std::string old_val;
        auto [found, x] = get(key, old_val, false, true);
        if (update && found && value == old_val)
            branch += "ignore"; // ignore update with the same value
        else if (found == update)
        {
            off_t fpos_values = lseek(fid_values, 0, SEEK_END);
            auto val = value;   // value is const
            Index entry;
            snprintf(entry.key, 256, "%s", key.c_str());
            entry.vfpos = fpos_values;
            entry.vsz = val.length();
            if (!found) // key not in cache and not in file
            {
                branch += "Insert";
                off_t fpos_keys = lseek(fid_keys, 0, SEEK_END);
                entry.index = fpos_keys / Index::entry_sz;
                entry.access(fid_keys, fid_values, &val, name, true); // write key to keys file and value to values file
                evict();
                values.push_front({val, entry.vfpos, 0});                 // load into cache
                keys.push_front({key, fpos_keys, values.begin()});
                x = xref.insert_or_assign(key, keys.begin()).first;
                entries++;
                inserts++;
            }
            else // found in cache
            {
                branch += "Update";
                Value& oldv = *x->second->it_value;
                bool inplace = val.length() <= old_val.length();
                spaces += old_val.length() - (inplace ? value.length() : 0);
                entry.index = x->second->fpos / Index::entry_sz;
                if (inplace)
                    entry.vfpos = fpos_values;
                entry.access(fid_keys, fid_values, &val, name, true); // Read index entry from file ignoring value
                values.erase(x->second->it_value);
                off_t fpos_keys = entry.index * Index::entry_sz;
                values.push_front({val, fpos_keys, (ssize_t)value.length()});
                keys.erase(x->second);
                keys.push_front({key, fpos_keys, values.begin()});
                x->second = keys.begin();
                updates++;
            }
        }
        if (trace_level & ((1 << ETrace::FILEIO) | (1 << ETrace::CACHE)))
            applog.info("CACHE|FILEIO: set {} {}={} {}", branch, key, value, update == found ? "ok" : "fail");
        return update == found;
    }
    operator std::string() const
    {
        return sstring(VERSION) + "\n" +
            "table: " + name + "\n" +
            Stats::operator::std::string() +
        format(
            "index entries: {}\n"
            "cache_entries: {}\n"
            "lock_cache: {}\n"
            "lock_fio: {}\n"
            "readers: {}\n"
            "value spaces: {}\n",
            entries,
            xref.size(),
            lock_cache,
            lock_fio,
            readers,
            spaces
        );
    }
    static bool self_test(int test, int loop, std::string &s)
    {
        bool ok = true;
        sstring t;
       #define LOG s+= t+"\n"; if (trace_level & (1 << ETrace::TEST)) applog.info("TEST: {}", t)
        t = format("Starting test {}, id {}...", test, self_tests++); LOG;
        if (loop>1)
            for (int i=0; i<loop; i++)
            {
                std::string localt;
                bool local_ok = ok && mode_async ?
                    co_self_test(test, 1, localt).get() :
                    self_test(test, 1, localt);
                if (local_ok != ok)
                {
                    t = localt;
                    LOG;
                }
                ok = local_ok;
            }
        else
        {
            Lock l(tables_lock);
            auto table_name = std::string(Table::master_table_name);
            t ="Dropping and using table " + table_name; LOG;
            auto it_table = tables.end();
            {
                it_table = tables.find(table_name);
                if (it_table == tables.end())
                    it_table = tables.emplace(table_name, new Table(table_name)).first;
                auto &table = *it_table->second;
                t = "Using table " + table_name; LOG;
                t = table.purge(true); LOG;
                t = table.close(true); LOG;
                Table::tables.erase(it_table);
                t = "Dropping table " + table_name; LOG;
                it_table = tables.emplace(table_name, new Table(table_name)).first;
                t = "(Re)create table " + table_name; LOG;
            }
            auto &table = *it_table->second;
            if (test==1)
            {
                std::string keys[] = {"aaa", "bbb"};
                std::string values0[] = {"_aaa_", "_bbb_"};
                for (int i=0; auto key : keys)
                    if (auto value = values0[i++]; !table.set(key, value, false))   // insert (key=value)s
                    {
                        t = format("Failed to insert {}={}", key, value); LOG;
                        ok = false;
                    }
                for (int i=0; auto key : keys)                                      // retrieve values of keys
                {
                    std::string value;
                    if (!table.get(key, value).found)  // read
                    {
                        t = format("Failed to retrieve {}", key); LOG;
                        ok = false;
                    }
                    if (value!=values0[i])
                    {
                        t = format("Unexpected value for key {}={} <> {}", key, value, values0[i]); LOG;
                        ok = false;
                    }
                    i++;
                }
                if (std::string value; !table.get(keys[0], value, true).found) // delete
                {
                    t = format("Delete failed for key {}", keys[0]); LOG;
                    ok = false;
                }
                for (int i=0; auto key : keys)                                      // retrieve values of keys
                {
                    std::string value;
                    bool found = table.get(key, value).found;  // read
                    if (i==0)
                    {
                        if (found)
                        {
                            t = format("Key {} was not deleted", key); LOG;
                            ok = false;
                        }
                    }
                    else if (value!=values0[i])
                    {
                        t = format("Unexpected value for key {}={} <> {}", key, value, values0[i]); LOG;
                        ok = false;
                    }
                    i++;
                }
            }
            else
            {
                std::string keys[] = {"aaa", "bbb", "ccc", "null"};
                std::string values0[] = {"_aaa_", "_bbb_", "_ccc_", ""};
                for (int i=0; auto key : keys)
                    if (auto value = values0[i++]; !table.set(key, value, false))   // insert (key=value)s
                    {
                        t = format("Failed to insert {}={}", key, value); LOG;
                        ok = false;
                    }
                for (int i=0; auto key : keys)                                      // retrieve values of keys
                {
                    std::string value;
                    if (!table.get(key, value).found)  // read
                    {
                        t = format("Failed to retrieve {}", key); LOG;
                        ok = false;
                    }
                    if (value!=values0[i])
                    {
                        t = format("Unexpected value for key {}={} <> {}", key, value, values0[i]); LOG;
                        ok = false;
                    }
                    i++;
                }
                for (int i=0; auto key : keys)
                    if (auto value = values0[i++]; table.set(key, value, false))    // try to overinsert (key=value)s
                    {
                        t = format("Failed to prevent overinsertion on pre-existing key {}={}", key, value); LOG;
                        ok = false;
                    }
                std::string values1[] = {"_AAAA_", "_BBB_", "_CC_", "_null_"};
                for (int i=0; auto key : keys)
                    if (auto value = values1[i++]; !table.set(key, value, true))    // update (key=value)s
                    {
                        t = format("Failed to update existing key {}={}", key, value); LOG;
                        ok = false;
                    }
                if (auto key = "ddd", value="_ddd_"; table.set(key, value, true))  // try to update non-existent key
                {
                    t = format("Failed to prevent updating of non-existent key {}={}", key, value); LOG;
                    ok = false;
                }
                for (int i=0; auto key : keys)
                {
                    if (std::string value; !table.get(key, value).found || value!=values1[i])
                    {
                        t = format("Different value for key {}={}, expected {}", key, value, values1[i]); LOG;
                        ok = false;
                    }
                    i++;
                }
                if (table.spaces==0)
                {
                    t = format("Table {} expected to have {} internal fragmentation", table.name, "non-zero"); LOG;
                }
                t = table.compact(); LOG;   // Compacting must reorder the values file.
                for (int i=0; auto key : keys)
                {
                    if (i%2) // delete every second key
                        if (std::string value; !table.get(key, value, true).found) // delete key
                        {
                            t = format("Delete failed for key {}={}, expected {}", key, value, values1[i]); LOG;
                            ok = false;
                        }
                    i++;
                }
                t = table.compact(); LOG; // Compacting remove all index entries marked as "deleted" and reorders the value file.
                if (auto count = (sizeof(keys)/sizeof(keys[0])+1) / 2; table.entries != count)
                {
                    t = format("Unexpected number of keys in table {}: {}, expected {}", table.name, table.entries, count); LOG;
                    ok = false;
                }
                t = table.list(true).get(); LOG; // revolve though index keys once
            }
        }
        t = format("... test {} ==== {} ====.\n", test, ok ? "PASSED" : "FAILED") + Stats::get_static_profiles(); LOG;
        return ok;
    }
    static seastar::future<bool> co_self_test(int test, int loop, std::string &s)
    {
        co_return self_test(test, loop, s);
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

    static std::atomic_bool tables_lock; // One self-test through
private:
    std::atomic_bool lock_fio = false;   // file lock: mutually exclussive readers and writers
    std::atomic_bool lock_cache = false; // cache lock: shared readers, exclusive writers
    std::atomic_int  readers = 0;        // count of concurrent readers

    enum ECacheMode {WRITETHROUGH, WRITEBACK};
    int cache_mode = WRITETHROUGH;
    int fid_keys{0};
    int fid_values{0};
    size_t entries{0};
    size_t spaces{0};

    struct Value
    {
        std::string text;
        off_t fpos;
        ssize_t sz;
    };
    struct Key
    {
        std::string text;
        off_t fpos;
        std::deque<Value>::iterator it_value;
    };

    std::deque<Value> values;
    std::deque<Key> keys;
    std::map<std::string, std::deque<Key>::iterator> xref;

    std::string evict()
    {
        if (keys.size() == max_cache)
        {
            auto key = keys.back().text;
            xref.erase(xref.find(key));
            keys.pop_back();
            values.pop_back(); // Eviction
            if (trace_level & (1 << ETrace::CACHE))
                applog.info("CACHE: evict key {}", key);
            evictions++;
            return "Eviction of key " + key;
        }
        return "Evition not required";
    }
    seastar::coroutine::experimental::generator<Index> index_revolver(bool locked, int &remove)
    {
        off_t fpos = 0;
        int count = 0;
        while (true)
        {
            char buf[Index::entry_sz+1];
            {
                Lock l(lock_cache, locked);
                lseek(fid_keys, fpos, SEEK_SET);
                auto len = read(fid_keys, buf, Index::entry_sz);
                if (len==0)
                {
                    if (trace_level & (1 << ETrace::FILEIO))
                        applog.info("FILEIO: EIF Reached the end of {}, count {}", db_prefix+name+keys_fname, count);
                    co_yield Index{.index = -count, .vfpos = Index::end_marker, .vsz = 0};
                    fpos = 0;
                    count = 0;
                    continue;
                }
                if (len<0)
                {
                    if (trace_level & (1 << ETrace::FILEIO))
                        applog.info("FILEIO: Error reading {}, count {}, errno {}", db_prefix+name+keys_fname, count, errno);
                    break;
                }
            }
            count++;
            char * sp = strchr(buf,',');
            Index ind{.index=int(fpos/(Index::entry_sz))};
            strncpy(ind.key, buf, sp-buf);
            ind.key[sp-buf]=0;
            sscanf(sp+1, "%jd,%zu", &ind.vfpos, &ind.vsz);
            if (trace_level & (1 << ETrace::FILEIO))
                applog.info("FILEIO: {} [{}, {}, {}, {}] of index {}",
                    ind.vfpos==Index::deleted_marker ? "Skipping deleted" : "Yielding",
                    ind.index,
                    ind.vfpos,
                    ind.key,
                    ind.vsz,
                    db_prefix+name+keys_fname);
            if (ind.vfpos!=Index::deleted_marker)
                co_yield ind;
            fpos += Index::entry_sz;
        }
    }
};
std::map<std::string, Table*> Table::tables;
std::atomic_bool Table::tables_lock = false;
bool Table::mode_async = false;

class DefaultHandle : public httpd::handler_base {
public:
    auto build_help(sstring url, sstring host, sstring table_name)
    {
        sstring s;
        s += sstring("<h1>") + VERSION + "</h1>\n" +
            format("Global stats: {}<br/>\n", Stats::get_static_profiles()) +
            "<html>\n<head>\n<style>\n"
            "html *{font-size: 1em !important;}\n"
            "body  {background-color: white;}\n"
            "h1    {color: black;}\n"
            "span  {color: black; min-width: 25%; display: inline-block;}\n"
            "p     {color: red;background-color: FloralWhite;}\n"
            "a     {text-decoration: none;}"
            "</style>\n</head>\n<body>\n" +
            sstring("<h1>Global commands:</h1>\n<p>");
        struct {const char*label, *str; } args_global[] =
        {
            {"QUIT",                "quit"},
            {"trace level all ON",  "?trace_level=11111"},
            {"trace level all OFF", "?trace_level=00000"},
            {"list tables in use",  "?used"},
            {"mode async",          "?mode=async"},
            {"mode sync(default)",  "?mode=sync"},
            {"view console file",   "?file_console"},
            {"Prometeus server",    "metrics?__name__=http*"},
        };
        for (auto arg : args_global)
            s += sstring("<span>") + arg.label + ":</span> <a href=\"" + host + "/" + arg.str + "\">" + arg.str + "</a><br/>\n";
        s += sstring("</p>\n<h1>Commands for table: " + table_name + "&nbsp</h1>\n"
            "For any of the following commands to work, a table must be created, or opened if it exists, with the <strong><a href=\"" + host + "/?use\">?use</a></strong> command.\n"
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
            {"(co)index next",          "?rownext"},
            {"insert aaa=_aaa_",        "?op=insert&key=aaa&value=_aaa_"},
            {"insert bbb=_bbb_",        "?op=insert&key=bbb&value=_bbb_"},
            {"insert ccc=_ccc_",        "?op=insert&key=ccc&value=_ccc_"},
            {"insert null=<NULL>",      "?op=insert&key=null&value="},
            {"query aaa",               "?key=aaa"},
            {"query bbb",               "?key=bbb"},
            {"query ccc",               "?key=ccc"},
            {"query null",              "?key=null"},
            {"update aaa=_aaaa_",       "?op=update&key=aaa&value=_aaaa_"},
            {"update bbb=_BBB_",        "?op=update&key=bbb&value=_BBB_"},
            {"update ccc=_cc_",         "?op=update&key=ccc&value=_cc_"},
            {"update null=_null_",      "?op=update&key=null&value=_null_"},
            {"update null=<null>",      "?op=update&key=null&value="},
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
            s += sstring("<span>") + arg.label + ":</span> <a href=\"" + surl + arg.str + "\">" + arg.str + "</a><br/>\n";
        }
        struct {const char*label, *str; } args_tests[] =
        {
            {"Test1: 2 inserts, 1 delete",   "?self_test=1"},
            {"Test2: 4-keys composite",      "?self_test=2"},
            {"Test2*10",                     "?self_test=2&loop=10"},
            {"Test2*100",                    "?self_test=2&loop=100"},
            {"Test2*1000",                   "?self_test=2&loop=1000"},
        };
        s += sstring("</p>\n<h1>Self tests:""&nbsp</h1>\n");
        for (auto arg : args_tests)
            s += sstring("<span>") + arg.label + ":</span> <a href=\"" + host + "/" + arg.str + "\">" + arg.str + "</a><br/>\n";
        s += "</p></span>\n</body>";
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
        bool plain = true;
        auto it_table = Table::tables.end();
        Stats::requests++;
        bool is_console_dump = false;
        if (req->query_parameters.size()==0) // HELP
        {
            s += build_help(url, host, table_name);
            plain = false;
        }
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
        }
        else if (req->query_parameters.contains("mode"))
        {
            auto level = req->query_parameters.at("mode");
            bool old_mode = Table::mode_async;
            bool new_mode = old_mode;
            if (level=="async")
                new_mode=true;
            else if (level=="sync")
                new_mode=false;
            s = format("Changing mode from {} to {}", old_mode ? "async" : "sync", new_mode ? "async" : "sync");
            Table::mode_async = new_mode;
        }
        else if (req->query_parameters.contains("file_console"))
        {
            auto fname = Table::db_prefix + std::string("") + Table::console_fname;
            auto fid_console = open(fname.c_str(), O_RDONLY, S_IREAD|S_IWRITE | S_IRGRP|S_IWGRP | S_IROTH|S_IWOTH);
            std::string text;
            bool err = true;
            if (fid_console>0)
            {
                auto fsize = lseek(fid_console, 0, SEEK_END);
                ssize_t max_size = 0x100000;
                ssize_t fpos = (max_size < fsize) ? (fsize - max_size) : 0;
                if (fsize-fpos < max_size)
                    max_size = fsize - fpos;
                lseek(fid_console, fpos, SEEK_SET);
                text.resize(max_size);
                if (::read(fid_console, (char*)text.c_str(), max_size) != max_size)
                    text = format("Error {} reading console file {} {} bytes from {} to {}\n", errno, fname, max_size, fpos, fsize) + text;
                else
                    err = false;
                ::close(fid_console);
            }
            else
                text = format("Error {} opening console file {}", errno, fname);
            if (err)
                if (trace_level & (1 << ETrace::SERVER))
                    applog.info("SERVER: {}{}{}", s, "console", text);
            s += text;
            plain = true;
            is_console_dump = true;
        }
        else if (req->query_parameters.contains("use"))
        {
            Lock l(Table::tables_lock);
            it_table = Table::tables.find(table_name);
            if (it_table == Table::tables.end())
            {
                auto & table = *(Table::tables.emplace(table_name, new Table(table_name)).first->second);
                s += "Created table " + table_name + ".\n";
            }
        }
        else if (req->query_parameters.contains("used"))
        {
            Lock l(Table::tables_lock);
            for (auto t : Table::tables) s += t.first + "\n";
        }
        else if (req->query_parameters.contains("self_test"))
        {
            auto test = req->query_parameters.at("self_test");
            int tn;
            sscanf(test.c_str(),"%d", &tn);
            auto loop = req->query_parameters.contains("loop") ? req->query_parameters.at("loop") : "1";
            int l;
            sscanf(loop.c_str(),"%d", &l);
            std::string text;
            bool result = Table::mode_async ?
                co_await Table::co_self_test(tn, l, text) :
                Table::self_test(tn, l, text);
            if (!plain)
            {
                std::string::size_type n = 0;
                const std::string rwhat = "\n";
                const std::string rwith = "<br/>\n";
                while ((n = text.find(rwhat, n)) != std::string::npos)
                {
                    text.replace( n, rwhat.size(), rwith);
                    n += rwith.size();
                }
            }
            s += text;
        }
        else
        {
            {
                Lock l(Table::tables_lock);
                it_table = Table::tables.find(table_name);
                if (it_table == Table::tables.end())
                    s += "Table " + table_name + " is not in use.\n";
            }
            std::string new_val;
            if (it_table != Table::tables.end())
            {
                auto &table = *it_table->second;
                if (req->query_parameters.contains("list"))
                    s = co_await table.list(true);
                else if (req->query_parameters.contains("invalidate"))
                    s = table.purge(false);
                else if (req->query_parameters.contains("compact"))
                    s = table.compact();
                else if (req->query_parameters.contains("purge"))
                    s = table.purge(true);
                else if (req->query_parameters.contains("stats"))
                    s += sstring(s.length() > 0 ? "\n" : "") + format("{}", (std::string)table);
                else if (req->query_parameters.contains("drop"))
                {
                    s += table.purge(true) + "\n" + table.close(true);
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
                        "Changing max_cache from {} to {}", old_max_cache, new_max_cache);
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
                    auto entry = co_await table.co_index_locked();
                    auto [index, key, vfpos, sz] = entry.value();
                    s = format("test index {}, key:{}, valpos:{}, valsz:{}, value:{}", index, key, vfpos, sz, "?");
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
            plain = true;
        }
        if (trace_level & (1 << ETrace::SERVER))
            applog.info("SERVER: DB request {} on table {}, {}", Stats::requests, table_name,
                        !plain ? "HTML help not shown in console" :
                        is_console_dump ? "Console log is not re-logged" :
                        s);
        rep->_content = s;
        rep->done(plain ? "plain" : "html");
        co_return co_await make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
    }
} defaultHandle;

class QuitHandle : public httpd::handler_base {
public:
    virtual future<std::unique_ptr<http::reply> > handle(const sstring& path,
        std::unique_ptr<http::request> req, std::unique_ptr<http::reply> rep)
    {
        // Need to quit the reactor, but also back the web browser.
        // Set a future to trigger after the page is sent-out for rendering
        auto rv = seastar::sleep(1s).then([] {
            if (trace_level & (1 << ETrace::SERVER))
                applog.info("SERVER: exiting...");
            // This doesn't do the job: seastar::engine().exit(0); // gracefully...
            exit(0); // not so gracefully...
        });
        rep->_content = "<body onload=\"history.back()\"\n<h1>Server shutting down</h1>\n</body>";
        rep->done("html");
        return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
    }
} quitHandle;

void set_routes(routes& r) {
    r.add_default_handler(&defaultHandle);
    r.add(operation_type::GET, url("/quit"), &quitHandle);
    r.add(operation_type::GET, url("/file").remainder("path"), new directory_handler("/"));
}

int main(int ac, char** av) {
    applog.info("{}", VERSION);
    prometheus::config pctx;
    app_template app;

    app.add_options()("port", bpo::value<uint16_t>()->default_value(10000), "HTTP Server port");
    app.add_options()("address", bpo::value<sstring>()->default_value("0.0.0.0"), "DB server address");
    app.add_options()("prefix", bpo::value<sstring>()->default_value("seastar_httpd"), "Prometheus metrics prefix");

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
            http_server_control server;
            uint16_t port = config["port"].as<uint16_t>();
            server.start("prometheus").get();
            prometheus::start(server, {.metric_help = "seastar::httpd server statistics", .prefix = config["prefix"].as<sstring>()}).get();
            net::inet_address addr(config["address"].as<sstring>());
            auto rb = make_shared<api_registry_builder>("apps/httpd/");
            server.set_routes(set_routes).get();
            server.set_routes([rb](routes& r){rb->set_api_doc(r);}).get();
            server.set_routes([rb](routes& r) {rb->register_function(r, "demo", "hello world application");}).get();
            server.listen(port).handle_exception([addr, port] (auto ep) {
                std::cerr << seastar::format("Could not start DB server on {}:{}: {}\n", addr, port, ep);
                return make_exception_future<>(ep);
            }).get();
            std::cout << VERSION << " listening on port " << port << " ...\n";
            stop_signal.wait().get();
            return 0;
        });
    });
}
