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
 * Modified on: 2024.04.11
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
 * Limitations:
 * - To keep the index human-readable, a CSV format was chosen: (key-name,off_values,size_value, padding).
 * - This format implies that keys must not contain comma characters.
 * - The empty string in queries has special meaning. Keys cannot be empty.
 * - Empty values should be allowed, but the REST API curerntly treats "" as an intention to delete the key.
 */

#include <seastar/http/httpd.hh>
#include <seastar/http/handlers.hh>
#include <seastar/http/function_handlers.hh>
#include <seastar/http/file_handler.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/reactor.hh>
//#include "demo.json.hh"
#include <seastar/http/api_docs.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/prometheus.hh>
#include <seastar/core/print.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/util/defer.hh>

namespace bpo = boost::program_options;

using namespace seastar;
using namespace httpd;

const char* available_trace_levels[] = {"LOCKS", "FILEIO", "CACHE", "SERVER"};
enum ETrace { LOCKS, FILEIO, CACHE, SERVER, MAX};
int trace_level = (1 << ETrace::FILEIO);

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
    constexpr static int version{0};
    std::atomic_int ops{0}, inserts{0}, deletes{0}, updates{0}, queries{0}, purges{0}, invalidates{0}, evictions{0}, compacts{0};
    operator sstring() const
    {
        return format(
            "ops:{}, "
            "inserts:{}, "
            "deletes:{}, "
            "updates:{}, "
            "queries:{}, "
            "purges:{}, "
            "invalidates:{}, "
            "evictions:{}, "
            "compacts:{}"
            ,
            ops.load(),
            inserts.load(),
            deletes.load(),
            updates.load(),
            queries.load(),
            purges.load(),
            invalidates.load(),
            evictions.load(),
            compacts.load()
        );
    }
};

struct Table : Stats {
    static size_t max_cache;
    std::string name;
    constexpr static auto keys_fname = "keys.txt";
    constexpr static auto values_fname = "values.txt";
    constexpr static auto compact_fname = "compact.txt";
    constexpr static size_t key_line_size = 256+1+20+10+1+1; // Key,int64,int32\n0
    std::atomic_bool lock = false;

    int fid_keys{0};
    int fid_values{0};
    size_t entries{0};
    size_t gaps{0};

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
    std::deque<Value> values;
    std::deque<Key> keys;
    std::map<std::string, std::deque<Key>::iterator> xref;

    Table(std::string name) : name(name)
    {
        fid_keys = open((name+keys_fname).c_str(), O_CREAT | O_RDWR, S_IREAD|S_IWRITE|S_IRGRP|S_IROTH);
        applog.info("Opened {:} returned {:}, {:}", name+keys_fname, fid_keys, errno);
        fid_values = open((name+values_fname).c_str(), O_CREAT | O_RDWR, S_IREAD|S_IWRITE|S_IRGRP|S_IROTH);
        applog.info("Opened {:} returned {:}", name+keys_fname, fid_values, errno);
    }
    auto list()
    {
        Lock l(lock);
        std::set<std::string> keys;
        char buf[key_line_size];
        lseek(fid_keys, 0, SEEK_SET);
        while (true)
        {
            auto len = read(fid_keys, buf, sizeof(buf)-1);
            if (len<=0)
                break;
            char * sp = strchr(buf,',');
            *sp++=0;
            keys.insert(buf);
        }
        std::string s;
        for (auto key : keys)
            s += key + "\n";
        if (trace_level & (1 << ETrace::SERVER))
            applog.info("list: {:}", s);
        return s;
    }
    void purge(bool files)
    {
        Lock l(lock);
        values.clear();
        keys.clear();
        xref.clear();
        if (files)
        {
            entries = 0;
            gaps = 0;
            ftruncate(fid_values, 0);
            ftruncate(fid_keys, 0);
            purges++;
        }
        invalidates++;
        if (trace_level & ((1 << ETrace::CACHE) | (1 << ETrace::FILEIO)))
            applog.info("purge: cache{:}", files ? " and files" : "");
    }
    void compact()
    {
        Lock l(lock);
        purge(false);
        int problems = 0;
        auto fid_temp = open((name+compact_fname).c_str(), O_CREAT | O_TRUNC | O_RDWR /*| O_TMPFILE*/, S_IREAD|S_IWRITE|S_IRGRP|S_IROTH);
        if (fid_temp>0)
        {
            off_t fpos_keys = 0;
            off_t fpos_values = 0;
            off_t fpos_temp = 0;
            char buf[key_line_size];
            lseek(fid_keys, 0, SEEK_SET);
            while (true)
            {
                auto len = read(fid_keys, buf, sizeof(buf)-1);
                if (len<=0)
                    break;
                char * sp = strchr(buf,',');
                *sp++=0;
                size_t sz;
                sscanf(sp, "%jd,%zu", &fpos_values, &sz);
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
                if (len<=0)
                    break;
                write(fid_values, buf, sizeof(buf)-1);
                fpos_values += sizeof(buf)-1;
            }
            ftruncate(fid_values, fpos_temp);
            close(fid_temp);
            unlink((name+compact_fname).c_str());
            compacts++;
            gaps=0;
        }
        else
            problems |= 1;
        if (trace_level & (1 << ETrace::FILEIO))
            applog.info("compacted: {:}, problems: {:}, errno: {:}", name+compact_fname, problems, errno);
    }
    void evict()
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
        }
    }
    std::string get(std::string key, bool reentered=false, bool remove=false)
    {
        Lock l(lock, reentered);
        std::string val = "";
        if (key=="")
        {
            if (remove)
                purge(false);
            else
            {
                for (auto &x : xref)
                    val += x.first + "=" + x.second->it_value->text + "; ";
                if (trace_level & (1 << ETrace::CACHE))
                    applog.info("get: cache");
            }
        }
        else if (auto x = xref.find(key); x != xref.end())
        {
            val = x->second->it_value->text;
            if (remove)
            {
                values.erase(x->second->it_value);
                keys.erase(x->second);
                xref.erase(x);
                entries--;
            }
            if (trace_level & (1 << ETrace::CACHE))
                applog.info("{:} {:} {:}", remove ? "get: removed" : "get: ", key=="" ? "<ALL>" : key, val);
        }
        else
        {
            int problems = 0;
            char buf[key_line_size];
            off_t fpos_keys = lseek(fid_keys, 0, SEEK_SET);
            off_t fpos_values = -1;
            size_t sz;
            while (true)
            {
                auto len = read(fid_keys, buf, sizeof(buf)-1);
                if (len != sizeof(buf)-1)
                    break;
                char * sp = strchr(buf,',');
                *sp++=0;
                if (!strcmp(buf,key.c_str()))
                {
                    sscanf(sp, "%jd,%zu", &fpos_values, &sz);
                    break; // key found in index file
                }
                fpos_keys = lseek(fid_keys, 0, SEEK_CUR);
            }
            if (fpos_values!=-1) // Load key from index file
            {
                evict();
                lseek(fid_values, fpos_values, SEEK_SET);
                val.resize(sz);
                if (read(fid_values, (char*)val.c_str(), sz) != (ssize_t)sz)
                    problems |= 1; // Cannot read from values file
                values.push_front({val, fpos_values, sz});
                keys.push_front({buf, fpos_keys, values.begin()});
                x = xref.insert_or_assign(key, keys.begin()).first;
                if (trace_level & ((1 << ETrace::FILEIO) | (1 << ETrace::CACHE)))
                    applog.info("get: (cachefill) {:} {:} {:}", key, val, problems);
            }
            else
                if (trace_level & ((1 << ETrace::FILEIO) | (1 << ETrace::CACHE)))
                    applog.info("get: key {:} not found", key);
        }
        return val;
    }
    void set(std::string key, std::string val)
    {
        Lock l(lock, false);
        auto old_val = ""; //get(key, true);
        int problems = 0;
        std::string branch = "Skip";
        if (val != old_val)
        {
            bool overwrite = false;
            char buf[key_line_size];
            auto x = xref.find(key);
            if (x == xref.end())
            {
                evict();
                off_t fpos_keys = lseek(fid_keys, 0, SEEK_SET);
                off_t fpos_values = -1;
                size_t sz;
                while (true)
                {
                    auto len = read(fid_keys, buf, sizeof(buf)-1);
                    if (len<=0)
                        break;
                    char * sp = strchr(buf,',');
                    *sp++=0;
                    if (!strcmp(buf,key.c_str()))
                    {
                        sscanf(sp, "%jd,%zu", &fpos_values, &sz);
                        break; // key found in index file
                    }
                    fpos_keys = lseek(fid_keys, 0, SEEK_CUR);
                }
                if (fpos_values == -1) // Load old key from index file
                {
                    fpos_values = lseek(fid_values, 0, SEEK_END);
                    values.push_front({val, fpos_values, 0});
                    keys.push_front({key, fpos_keys, values.begin()});
                    x = xref.insert_or_assign(key, keys.begin()).first;
                    branch = "Add";
                    entries++;
                }
                else
                {
                    values.push_front({val, fpos_values, sz});
                    keys.push_front({buf, fpos_keys, values.begin()});
                    x = xref.insert_or_assign(key, keys.begin()).first;
                    branch = "Fill";
                }
            }
            else
            {
                branch = "Overwrite";
                overwrite = true;
            }
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
                    gaps += sz-val.length();
                    fpos_values = oldv.fpos;
                }
                else
                    gaps += sz;
                sz = val.length();
                fpos_values = lseek(fid_values, fpos_values, reuse_value_space ? SEEK_SET : SEEK_END);
                if (write(fid_values, val.c_str(), val.length()) != (ssize_t)val.length())
                    problems |= 1; // Cannot write to values file
                values.erase(x->second->it_value);
                values.push_front({val, fpos_values, sz});
                sprintf(buf, "%s,%jd,%zu,", key.c_str(), fpos_values, sz);
                auto len = strlen(buf);
                auto fpos_keys = x->second->fpos;
                lseek(fid_keys, x->second->fpos, SEEK_SET);
                if (write(fid_keys, buf, len) != (ssize_t)len)
                    problems |= 2; // Cannot write to keys file
                len = sizeof(buf)-1-len-1;
                sprintf(buf, "%*c\n", int(len), ' ');
                len++;
                if (write(fid_keys, buf, len) != (ssize_t)len)
                    problems |= 2; // Cannot write to keys file
                keys.erase(x->second);
                keys.push_front({key, fpos_keys, values.begin()});
                x->second = keys.begin();
            }
        }
        if (trace_level & ((1 << ETrace::FILEIO) | (1 << ETrace::CACHE)))
            applog.info("set: {:} {:} {:} problems: {:}", branch, key, val, problems);
    }
} table {"table_"};
size_t Table::max_cache = 2;


//class handl : public httpd::handler_base {
//public:
//    virtual future<std::unique_ptr<http::reply> > handle(const sstring& path,
//            std::unique_ptr<http::request> req, std::unique_ptr<http::reply> rep) {
//        rep->_content = "hello";
//        rep->done("html");
//        return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
//    }
//};

void set_routes(routes& r) {
    function_handler* h1 = new function_handler([](const_req req) {
        auto key = req.query_parameters.contains("key") ? req.query_parameters.at("key") : "";
        auto value = req.query_parameters.contains("value") ? req.query_parameters.at("value") : "";
        auto op = req.query_parameters.contains("op") ? req.query_parameters.at("op") : "";
        if (op=="list")
            value = table.list();
        else if (op=="invalidate")
            table.purge(false);
        else if (op=="compact")
            table.compact();
        else if (op=="insert")
        {
            table.set(key, value);
            table.inserts++;
        }
        else if (op=="purge")
        {
            table.purge(true);
            table.deletes++;
        }
        else if (op=="delete")
        {
            table.get(key, true);
            table.deletes++;
        }
        else if (op=="update")
        {
            table.set(key, value);
            table.updates++;
        }
        else // any other op is a read
        {
            value = table.get(key);
            table.queries++;
        }
        std::string s = format("DB request {}", table.ops++);
        s += " op=" + op + " " + key + "=" + value;
        if (trace_level & (1 << ETrace::SERVER))
            applog.info("{:}", s);
        return s;
    });
    function_handler* h2 = new function_handler([](const_req req) {
        auto ss = format(
            "Stats:{}, "
            "Entries:{}, "
            "cachesz:{}, "
            "Gaps:{}, "
            "LockPasses:{}, "
            "LockCollisions:{} "
            ,
            (sstring)table,
            table.entries,
            table.xref.size(),
            table.gaps,
            Lock::passes,
            Lock::collisions
        );
        if (trace_level & (1 << ETrace::SERVER))
            applog.info("{:}", ss);
        return ss;
    });
    function_handler* h3 = new function_handler([](const_req req) {
        std::string s;
        if (req.query_parameters.contains("trace_level"))
        {
            auto level = req.query_parameters.at("trace_level");
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
        if (req.query_parameters.contains("max_cache"))
        {
            auto val = req.query_parameters.at("max_cache");
            int old_max_cache = Table::max_cache;
            int new_max_cache;
            sscanf("%d", val.c_str(), &new_max_cache);
            if (new_max_cache>=0)
            {
                table.purge(false);
                Table::max_cache = new_max_cache;
            }
            s = format("Changing max_cache from {:} to {:}", old_max_cache, new_max_cache);
        }
        if ((int)trace_level & (1 << ETrace::SERVER))
            applog.info("{:}", s);
        return s;
    });
    function_handler* h4 = new function_handler([](const_req req) {
        std::string s = "Server shutting down";
        exit(0);
        if (trace_level & (1 << ETrace::SERVER))
            applog.info("{:}", s);
        return s;
    });
    r.add(operation_type::GET, url("/"), h1);
    r.add(operation_type::GET, url("/stats"), h2);
    r.add(operation_type::GET, url("/config"), h3);
    r.add(operation_type::GET, url("/quit"), h4);
    r.add(operation_type::GET, url("/jf"), h2);
    r.add(operation_type::GET, url("/file").remainder("path"), new directory_handler("/"));
}

int main(int ac, char** av) {
    applog.info("Db demo {:}", table.version);
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
