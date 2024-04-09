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
 * Date: 2024.04.08
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

logger applog("app");

struct Stats {
    constexpr static int version{0};
    std::atomic_int sr;
    std::atomic_int ops;
    std::atomic_int inserts, deletes, updates, queries;
};

struct Lock {
    std::atomic_bool l {false};
    Lock()
    {
        bool expected = false;
        while (!atomic_compare_exchange_strong(&l, &expected, true))
            collisions++;
        applog.info("Locked");
    }
    ~Lock(){ applog.info("Unlocked"); l = false; }
    static std::atomic_int collisions;
};
std::atomic_int Lock::collisions = 0;

struct Cache : Stats {
    std::map<std::string, std::string> key_vals;
    std::string get(std::string key, bool remove=false) {
        Lock l;
        std::string value = "";
        if (key=="")
        {
            if (remove)
                key_vals.clear();
            else
                for (auto place : key_vals)
                    value += place.first + "=" + place.second + "; ";
            applog.info("{:} {:}", remove ? "Removed" : "Get", "<ALL>");
        }
        else if (auto place = key_vals.find(key); place != key_vals.end()) {
            value = place->second;
            if (remove)
                key_vals.erase(place);
            applog.info("{:} {:}", remove ? "Removed" : "Get", key=="" ? "<ALL>" : key);
        }
        return value;
    }
    void set(std::string key, std::string val) {
        Lock l;
        key_vals.insert_or_assign(key, val);
        applog.info("{:} {:} {:}", "Set", key, val);
    }
} cache {{0,0, 0,0,0,0}};

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
        if (op=="insert")
        {
            cache.set(key, value);
            cache.inserts++;
        }
        else if (op=="delete")
        {
            cache.get(key, true);
            cache.deletes++;
        }
        else if (op=="update")
        {
            cache.set(key, value);
            cache.updates++;
        }
        else // any op other than insert, delete, or update is a read
        {
            value = cache.get(key);
            cache.queries++;
        }
        std::string s = format("DB request {}", cache.ops++);
        s += " op=" + op + " " + key + "=" + value;
        applog.info("{:}", s);
        return s;
    });
    function_handler* h2 = new function_handler([](const_req req) {
        std::string s = format(
            "Stats: {} "
            "ops:{}, "
            "inserts:{}, "
            "deletes:{}, "
            "updates:{}, "
            "queries:{}, "
            "cachesz:{}, "
            "Collisions:{}, "
            ,
            cache.sr.load(),
            cache.ops.load(),
            cache.inserts.load(),
            cache.deletes.load(),
            cache.updates.load(),
            cache.queries.load(),
            cache.key_vals.size(),
            Lock::collisions
        );
        cache.sr++;
        applog.info("{:}", s);
        return sstring(s);
    });
    r.add(operation_type::GET, url("/"), h1);
    r.add(operation_type::GET, url("/stats"), h2);
    r.add(operation_type::GET, url("/jf"), h2);
    r.add(operation_type::GET, url("/file").remainder("path"), new directory_handler("/"));
}

int main(int ac, char** av) {
    applog.info("Db demo {:}", cache.version);
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
