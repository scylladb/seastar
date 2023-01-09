/*
 * Copyright 2015 Cloudius Systems
 */

#include <seastar/http/httpd.hh>
#include <seastar/http/handlers.hh>
#include <seastar/http/matcher.hh>
#include <seastar/http/matchrules.hh>
#include <seastar/json/formatter.hh>
#include <seastar/http/routes.hh>
#include <seastar/http/exception.hh>
#include <seastar/http/transformers.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/when_all.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include "loopback_socket.hh"
#include <boost/algorithm/string.hpp>
#include <seastar/core/thread.hh>
#include <seastar/util/noncopyable_function.hh>
#include <seastar/http/json_path.hh>
#include <seastar/http/response_parser.hh>
#include <sstream>
#include <seastar/core/shared_future.hh>
#include <seastar/http/client.hh>
#include <seastar/http/url.hh>
#include <seastar/util/later.hh>
#include <seastar/util/short_streams.hh>

using namespace seastar;
using namespace httpd;

class handl : public httpd::handler_base {
public:
    virtual future<std::unique_ptr<http::reply> > handle(const sstring& path,
            std::unique_ptr<http::request> req, std::unique_ptr<http::reply> rep) {
        rep->done("html");
        return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
    }
};

SEASTAR_TEST_CASE(test_reply)
{
    http::reply r;
    r.set_content_type("txt");
    BOOST_REQUIRE_EQUAL(r._headers["Content-Type"], sstring("text/plain"));
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_str_matcher)
{

    str_matcher m("/hello");
    parameters param;
    BOOST_REQUIRE_EQUAL(m.match("/abc/hello", 4, param), 10u);
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_param_matcher)
{

    param_matcher m("param");
    parameters param;
    BOOST_REQUIRE_EQUAL(m.match("/abc/hello", 4, param), 10u);
    BOOST_REQUIRE_EQUAL(param.path("param"), "/hello");
    BOOST_REQUIRE_EQUAL(param["param"], "hello");
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_match_rule)
{

    parameters param;
    handl* h = new handl();
    match_rule mr(h);
    mr.add_str("/hello").add_param("param");
    httpd::handler_base* res = mr.get("/hello/val1", param);
    BOOST_REQUIRE_EQUAL(res, h);
    BOOST_REQUIRE_EQUAL(param["param"], "val1");
    res = mr.get("/hell/val1", param);
    httpd::handler_base* nl = nullptr;
    BOOST_REQUIRE_EQUAL(res, nl);
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_match_rule_order)
{
    parameters param;
    routes route;

    handl* h1 = new handl();
    route.add(operation_type::GET, url("/hello"), h1);

    handl* h2 = new handl();
    route.add(operation_type::GET, url("/hello"), h2);

    auto rh = route.get_handler(GET, "/hello", param);
    BOOST_REQUIRE_EQUAL(rh, h1);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_put_drop_rule)
{
    routes rts;
    auto h = std::make_unique<handl>();
    parameters params;

    {
        auto reg = handler_registration(rts, *h, "/hello", operation_type::GET);
        auto res = rts.get_handler(operation_type::GET, "/hello", params);
        BOOST_REQUIRE_EQUAL(res, h.get());
    }

    auto res = rts.get_handler(operation_type::GET, "/hello", params);
    httpd::handler_base* nl = nullptr;
    BOOST_REQUIRE_EQUAL(res, nl);
    return make_ready_future<>();
}

// Putting a duplicated exact rule would result
// in a memory leak due to the fact that rules are implemented
// as raw pointers. In order to prevent such leaks,
// an exception is thrown if somebody tries to put
// a duplicated rule without removing the old one first.
// The interface demands that the callee allocates the handle,
// so it should also expect the callee to free it before
// overwriting.
SEASTAR_TEST_CASE(test_duplicated_exact_rule)
{
    parameters param;
    routes route;

    handl* h1 = new handl;
    route.put(operation_type::GET, "/hello", h1);

    handl* h2 = new handl;
    BOOST_REQUIRE_THROW(route.put(operation_type::GET, "/hello", h2), std::runtime_error);

    delete route.drop(operation_type::GET, "/hello");
    route.put(operation_type::GET, "/hello", h2);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_add_del_cookie)
{
    routes rts;
    handl* h = new handl();
    match_rule mr(h);
    mr.add_str("/hello");
    parameters params;

    {
        auto reg = rule_registration(rts, mr, operation_type::GET);
        auto res = rts.get_handler(operation_type::GET, "/hello", params);
        BOOST_REQUIRE_EQUAL(res, h);
    }

    auto res = rts.get_handler(operation_type::GET, "/hello", params);
    httpd::handler_base* nl = nullptr;
    BOOST_REQUIRE_EQUAL(res, nl);
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_formatter)
{
    BOOST_REQUIRE_EQUAL(json::formatter::to_json(true), "true");
    BOOST_REQUIRE_EQUAL(json::formatter::to_json(false), "false");
    BOOST_REQUIRE_EQUAL(json::formatter::to_json(1), "1");
    const char* txt = "efg";
    BOOST_REQUIRE_EQUAL(json::formatter::to_json(txt), "\"efg\"");
    sstring str = "abc";
    BOOST_REQUIRE_EQUAL(json::formatter::to_json(str), "\"abc\"");
    float f = 1;
    BOOST_REQUIRE_EQUAL(json::formatter::to_json(f), "1");
    f = 1.0/0.0;
    BOOST_CHECK_THROW(json::formatter::to_json(f), std::out_of_range);
    f = -1.0/0.0;
    BOOST_CHECK_THROW(json::formatter::to_json(f), std::out_of_range);
    f = 0.0/0.0;
    BOOST_CHECK_THROW(json::formatter::to_json(f), std::invalid_argument);
    double d = -1;
    BOOST_REQUIRE_EQUAL(json::formatter::to_json(d), "-1");
    d = 1.0/0.0;
    BOOST_CHECK_THROW(json::formatter::to_json(d), std::out_of_range);
    d = -1.0/0.0;
    BOOST_CHECK_THROW(json::formatter::to_json(d), std::out_of_range);
    d = 0.0/0.0;
    BOOST_CHECK_THROW(json::formatter::to_json(d), std::invalid_argument);
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_decode_url) {
    http::request req;
    req._url = "/a?q=%23%24%23";
    sstring url = req.parse_query_param();
    BOOST_REQUIRE_EQUAL(url, "/a");
    BOOST_REQUIRE_EQUAL(req.get_query_param("q"), "#$#");
    req._url = "/a?a=%23%24%23&b=%22%26%22";
    req.parse_query_param();
    BOOST_REQUIRE_EQUAL(req.get_query_param("a"), "#$#");
    BOOST_REQUIRE_EQUAL(req.get_query_param("b"), "\"&\"");
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_routes) {
    handl* h1 = new handl();
    handl* h2 = new handl();
    routes route;
    route.add(operation_type::GET, url("/api").remainder("path"), h1);
    route.add(operation_type::GET, url("/"), h2);
    std::unique_ptr<http::request> req = std::make_unique<http::request>();
    std::unique_ptr<http::reply> rep = std::make_unique<http::reply>();

    auto f1 =
            route.handle("/api", std::move(req), std::move(rep)).then(
                    [] (std::unique_ptr<http::reply> rep) {
                        BOOST_REQUIRE_EQUAL((int )rep->_status, (int )http::reply::status_type::ok);
                    });
    req.reset(new http::request);
    rep.reset(new http::reply);

    auto f2 =
            route.handle("/", std::move(req), std::move(rep)).then(
                    [] (std::unique_ptr<http::reply> rep) {
                        BOOST_REQUIRE_EQUAL((int )rep->_status, (int )http::reply::status_type::ok);
                    });
    req.reset(new http::request);
    rep.reset(new http::reply);
    auto f3 =
            route.handle("/api/abc", std::move(req), std::move(rep)).then(
                    [] (std::unique_ptr<http::reply> rep) {
                    });
    req.reset(new http::request);
    rep.reset(new http::reply);
    auto f4 =
            route.handle("/ap", std::move(req), std::move(rep)).then(
                    [] (std::unique_ptr<http::reply> rep) {
                        BOOST_REQUIRE_EQUAL((int )rep->_status,
                                            (int )http::reply::status_type::not_found);
                    });
    return when_all(std::move(f1), std::move(f2), std::move(f3), std::move(f4))
            .then([] (std::tuple<future<>, future<>, future<>, future<>> fs) {
        std::get<0>(fs).get();
        std::get<1>(fs).get();
        std::get<2>(fs).get();
        std::get<3>(fs).get();
    });
}

SEASTAR_TEST_CASE(test_json_path) {
    shared_ptr<bool> res1 = make_shared<bool>(false);
    shared_ptr<bool> res2 = make_shared<bool>(false);
    shared_ptr<bool> res3 = make_shared<bool>(false);
    shared_ptr<routes> route = make_shared<routes>();
    path_description path1("/my/path",GET,"path1",
        {{"param1", path_description::url_component_type::PARAM}
        ,{"/text", path_description::url_component_type::FIXED_STRING}},{});
    path_description path2("/my/path",GET,"path2",
            {{"param1", path_description::url_component_type::PARAM}
            ,{"param2", path_description::url_component_type::PARAM}},{});
    path_description path3("/my/path",GET,"path3",
            {{"param1", path_description::url_component_type::PARAM}
            ,{"param2", path_description::url_component_type::PARAM_UNTIL_END_OF_PATH}},{});

    path1.set(*route, [res1] (const_req req) {
        (*res1) = true;
        BOOST_REQUIRE_EQUAL(req.param["param1"], "value1");
        return "";
    });

    path2.set(*route, [res2] (const_req req) {
        (*res2) = true;
        BOOST_REQUIRE_EQUAL(req.param["param1"], "value2");
        BOOST_REQUIRE_EQUAL(req.param["param2"], "text1");
        return "";
    });

    path3.set(*route, [res3] (const_req req) {
        (*res3) = true;
        BOOST_REQUIRE_EQUAL(req.param["param1"], "value3");
        BOOST_REQUIRE_EQUAL(req.param["param2"], "text2/text3");
        return "";
    });

    auto f1 = route->handle("/my/path/value1/text", std::make_unique<http::request>(), std::make_unique<http::reply>()).then([res1, route] (auto f) {
        BOOST_REQUIRE_EQUAL(*res1, true);
    });

    auto f2 = route->handle("/my/path/value2/text1", std::make_unique<http::request>(), std::make_unique<http::reply>()).then([res2, route] (auto f) {
        BOOST_REQUIRE_EQUAL(*res2, true);
    });

    auto f3 = route->handle("/my/path/value3/text2/text3", std::make_unique<http::request>(), std::make_unique<http::reply>()).then([res3, route] (auto f) {
        BOOST_REQUIRE_EQUAL(*res3, true);
    });

    return when_all(std::move(f1), std::move(f2), std::move(f3))
                .then([] (std::tuple<future<>, future<>, future<>> fs) {
            std::get<0>(fs).get();
            std::get<1>(fs).get();
            std::get<2>(fs).get();
    });
}

/*!
 * \brief a helper data sink that stores everything it gets in a stringstream
 */
class memory_data_sink_impl : public data_sink_impl {
    std::stringstream& _ss;
public:
    memory_data_sink_impl(std::stringstream& ss) : _ss(ss) {
    }
    virtual future<> put(net::packet data)  override {
        abort();
        return make_ready_future<>();
    }
    virtual future<> put(temporary_buffer<char> buf) override {
        _ss.write(buf.get(), buf.size());
        return make_ready_future<>();
    }
    virtual future<> flush() override {
        return make_ready_future<>();
    }

    virtual future<> close() override {
        return make_ready_future<>();
    }
};

class memory_data_sink : public data_sink {
public:
    memory_data_sink(std::stringstream& ss)
        : data_sink(std::make_unique<memory_data_sink_impl>(ss)) {}
};

future<> test_transformer_stream(std::stringstream& ss, content_replace& cr, std::vector<sstring>&& buffer_parts) {
    std::unique_ptr<seastar::http::request> req = std::make_unique<seastar::http::request>();
    ss.str("");
    req->_headers["Host"] = "localhost";
    output_stream_options opts;
    opts.trim_to_size = true;
    return do_with(output_stream<char>(cr.transform(std::move(req), "json", output_stream<char>(memory_data_sink(ss), 32000, opts))),
            std::vector<sstring>(std::move(buffer_parts)), [] (output_stream<char>& os, std::vector<sstring>& parts) {
        return do_for_each(parts, [&os](auto& p) {
            return os.write(p);
        }).then([&os] {
            return os.close();
        });
    });
}

SEASTAR_TEST_CASE(test_transformer) {
    return do_with(std::stringstream(), content_replace("json"), [] (std::stringstream& ss, content_replace& cr) {
        output_stream_options opts;
        opts.trim_to_size = true;
        return do_with(output_stream<char>(cr.transform(std::make_unique<seastar::http::request>(), "html", output_stream<char>(memory_data_sink(ss), 32000, opts))),
                [] (output_stream<char>& os) {
            return os.write(sstring("hello-{{Protocol}}-xyz-{{Host}}")).then([&os] {
                return os.close();
            });
        }).then([&ss, &cr] () {
            BOOST_REQUIRE_EQUAL(ss.str(), "hello-{{Protocol}}-xyz-{{Host}}");
            return test_transformer_stream(ss, cr, {"hell", "o-{", "{Pro", "tocol}}-xyz-{{Ho", "st}}{{Pr"}).then([&ss, &cr] {
                BOOST_REQUIRE_EQUAL(ss.str(), "hello-http-xyz-localhost{{Pr");
                return test_transformer_stream(ss, cr, {"hell", "o-{{", "Pro", "tocol}}{{Protocol}}-{{Protoxyz-{{Ho", "st}}{{Pr"}).then([&ss, &cr] {
                    BOOST_REQUIRE_EQUAL(ss.str(), "hello-httphttp-{{Protoxyz-localhost{{Pr");
                    return test_transformer_stream(ss, cr, {"hell", "o-{{Pro", "t{{Protocol}}ocol}}", "{{Host}}"}).then([&ss] {
                        BOOST_REQUIRE_EQUAL(ss.str(), "hello-{{Prothttpocol}}localhost");
                    });
                });
            });
        });
    });
}

struct http_consumer {
    std::map<sstring, std::string> _headers;
    std::string _body;
    uint32_t _remain = 0;
    std::string _current;
    char last = '\0';
    uint32_t _size = 0;
    bool _concat = true;

    enum class status_type {
        READING_HEADERS,
        CHUNK_SIZE,
        CHUNK_BODY,
        CHUNK_END,
        READING_BODY_BY_SIZE,
        DONE
    };
    status_type status = status_type::READING_HEADERS;

    bool read(const temporary_buffer<char>& b) {
        for (auto c : b) {
            if (last =='\r' && c == '\n') {
                if (_current == "") {
                    if (status == status_type::READING_HEADERS || (status == status_type::CHUNK_BODY && _remain == 0)) {
                        if (status == status_type::READING_HEADERS && _headers.find("Content-Length") != _headers.end()) {
                            _remain = stoi(_headers["Content-Length"], nullptr, 16);
                            if (_remain == 0) {
                                status = status_type::DONE;
                                break;
                            }
                            status = status_type::READING_BODY_BY_SIZE;
                        } else {
                            status = status_type::CHUNK_SIZE;
                        }
                    } else if (status == status_type::CHUNK_END) {
                        status = status_type::DONE;
                        break;
                    }
                } else {
                    switch (status) {
                    case status_type::READING_HEADERS: add_header(_current);
                    break;
                    case status_type::CHUNK_SIZE: set_chunk(_current);
                    break;
                    default:
                        break;
                    }
                    _current = "";
                }
                last = '\0';
            } else {
                if (last != '\0') {
                    if (status == status_type::CHUNK_BODY || status == status_type::READING_BODY_BY_SIZE) {
                        if (_concat) {
                            _body = _body + last;
                        }
                        _size++;
                        _remain--;
                        if (_remain <= 1 && status == status_type::READING_BODY_BY_SIZE) {
                            if (_concat) {
                                _body = _body + c;
                            }
                            _size++;
                            status = status_type::DONE;
                            break;
                        }
                    } else {
                        _current = _current + last;
                    }

                }
                last = c;
            }
        }
        return status == status_type::DONE;
    }

    void set_chunk(const std::string& s) {
        _remain = stoi(s, nullptr, 16);
        if (_remain == 0) {
            status = status_type::CHUNK_END;
        } else {
            status = status_type::CHUNK_BODY;
        }
    }

    void add_header(const std::string& s) {
        std::vector<std::string> strs;
        boost::split(strs, s, boost::is_any_of(":"));
        if (strs.size() > 1) {
            _headers[strs[0]] = strs[1];
        }
    }
};

class test_client_server {
public:
    static future<> write_request(output_stream<char>& output) {
        return output.write(sstring("GET /test HTTP/1.1\r\nHost: myhost.org\r\n\r\n")).then([&output]{
                return output.flush();
        });
    }

    static future<> run_test(std::function<future<>(output_stream<char> &&)>&& write_func, std::function<bool(size_t, http_consumer&)> reader) {
        return do_with(loopback_connection_factory(1), foreign_ptr<shared_ptr<http_server>>(make_shared<http_server>("test")),
                [reader, &write_func] (loopback_connection_factory& lcf, auto& server) {
            return do_with(loopback_socket_impl(lcf), [&server, &lcf, reader, &write_func](loopback_socket_impl& lsi) {
                httpd::http_server_tester::listeners(*server).emplace_back(lcf.get_server_socket());

                auto client = seastar::async([&lsi, reader] {
                    connected_socket c_socket = lsi.connect(socket_address(ipv4_addr()), socket_address(ipv4_addr())).get0();
                    input_stream<char> input(c_socket.input());
                    output_stream<char> output(c_socket.output());
                    bool more = true;
                    size_t count = 0;
                    while (more) {
                        http_consumer htp;
                        htp._concat = false;

                        write_request(output).get();
                        repeat([&input, &htp] {
                            return input.read().then([&htp](const temporary_buffer<char>& b) mutable {
                                return (b.size() == 0 || htp.read(b)) ? make_ready_future<stop_iteration>(stop_iteration::yes) :
                                        make_ready_future<stop_iteration>(stop_iteration::no);
                            });
                        }).get();
                        std::cout << htp._body << std::endl;
                        more = reader(count, htp);
                        count++;
                    }
                    if (input.eof()) {
                        input.close().get();
                    }
                });

                auto server_setup = seastar::async([&server, &write_func] {
                    class test_handler : public handler_base {
                        size_t count = 0;
                        http_server& _server;
                        std::function<future<>(output_stream<char> &&)> _write_func;
                        promise<> _all_message_sent;
                    public:
                        test_handler(http_server& server, std::function<future<>(output_stream<char> &&)>&& write_func) : _server(server), _write_func(write_func) {
                        }
                        future<std::unique_ptr<http::reply>> handle(const sstring& path,
                                std::unique_ptr<http::request> req, std::unique_ptr<http::reply> rep) override {
                            rep->write_body("json", std::move(_write_func));
                            count++;
                            _all_message_sent.set_value();
                            return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
                        }
                        future<> wait_for_message() {
                            return _all_message_sent.get_future();
                        }
                    };
                    auto handler = new test_handler(*server, std::move(write_func));
                    server->_routes.put(GET, "/test", handler);
                    when_all(server->do_accepts(0), handler->wait_for_message()).get();
                });
                return when_all(std::move(client), std::move(server_setup));
            }).discard_result().then_wrapped([&server] (auto f) {
                f.ignore_ready_future();
                return server->stop();
            });
        });
    }
    static future<> run(std::vector<std::tuple<bool, size_t>> tests) {
        return do_with(loopback_connection_factory(1), foreign_ptr<shared_ptr<http_server>>(make_shared<http_server>("test")),
                [tests] (loopback_connection_factory& lcf, auto& server) {
            return do_with(loopback_socket_impl(lcf), [&server, &lcf, tests](loopback_socket_impl& lsi) {
                httpd::http_server_tester::listeners(*server).emplace_back(lcf.get_server_socket());

                auto client = seastar::async([&lsi, tests] {
                    connected_socket c_socket = lsi.connect(socket_address(ipv4_addr()), socket_address(ipv4_addr())).get0();
                    input_stream<char> input(c_socket.input());
                    output_stream<char> output(c_socket.output());
                    bool more = true;
                    size_t count = 0;
                    while (more) {
                        http_consumer htp;
                        write_request(output).get();
                        repeat([&input, &htp] {
                            return input.read().then([&htp](const temporary_buffer<char>& b) mutable {
                                return (b.size() == 0 || htp.read(b)) ? make_ready_future<stop_iteration>(stop_iteration::yes) :
                                        make_ready_future<stop_iteration>(stop_iteration::no);
                            });
                        }).get();
                        if (std::get<bool>(tests[count])) {
                            BOOST_REQUIRE_EQUAL(htp._body.length(), std::get<size_t>(tests[count]));
                        } else {
                            BOOST_REQUIRE_EQUAL(input.eof(), true);
                            more = false;
                        }
                        count++;
                        if (count == tests.size()) {
                            more = false;
                        }
                    }
                    if (input.eof()) {
                        input.close().get();
                    }
                });

                auto server_setup = seastar::async([&server, tests] {
                    class test_handler : public handler_base {
                        size_t count = 0;
                        http_server& _server;
                        std::vector<std::tuple<bool, size_t>> _tests;
                        promise<> _all_message_sent;
                    public:
                        test_handler(http_server& server, const std::vector<std::tuple<bool, size_t>>& tests) : _server(server), _tests(tests) {
                        }
                        future<std::unique_ptr<http::reply>> handle(const sstring& path,
                                std::unique_ptr<http::request> req, std::unique_ptr<http::reply> rep) override {
                            rep->write_body("txt", make_writer(std::get<size_t>(_tests[count]), std::get<bool>(_tests[count])));
                            count++;
                            if (count == _tests.size()) {
                                _all_message_sent.set_value();
                            }
                            return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
                        }
                        future<> wait_for_message() {
                            return _all_message_sent.get_future();
                        }
                    };
                    auto handler = new test_handler(*server, tests);
                    server->_routes.put(GET, "/test", handler);
                    when_all(server->do_accepts(0), handler->wait_for_message()).get();
                });
                return when_all(std::move(client), std::move(server_setup));
            }).discard_result().then_wrapped([&server] (auto f) {
                f.ignore_ready_future();
                return server->stop();
            });
        });
    }

    static noncopyable_function<future<>(output_stream<char>&& o_stream)> make_writer(size_t len, bool success) {
        return [len, success] (output_stream<char>&& o_stream) mutable {
            return do_with(output_stream<char>(std::move(o_stream)), uint32_t(len/10), [success](output_stream<char>& str, uint32_t& remain) {
                if (remain == 0) {
                    if (success) {
                        return str.close();
                    } else {
                        throw std::runtime_error("Throwing exception before writing");
                    }
                }
                return repeat([&str, &remain] () mutable {
                    return str.write("1234567890").then([&remain]() mutable {
                        remain--;
                        return (remain == 0)? make_ready_future<stop_iteration>(stop_iteration::yes) : make_ready_future<stop_iteration>(stop_iteration::no);
                    });
                }).then([&str, success] {
                    if (!success) {
                        return str.flush();
                    }
                    return make_ready_future<>();
                }).then([&str, success] {
                    if (success) {
                        return str.close();
                    } else {
                        throw std::runtime_error("Throwing exception after writing");
                    }
                });
            });
        };
    }
};

SEASTAR_TEST_CASE(test_message_with_error_non_empty_body) {
    std::vector<std::tuple<bool, size_t>> tests = {
        std::make_tuple(true, 100),
        std::make_tuple(false, 10000)};
    return test_client_server::run(tests);
}

SEASTAR_TEST_CASE(test_simple_chunked) {
    std::vector<std::tuple<bool, size_t>> tests = {
        std::make_tuple(true, 100000),
        std::make_tuple(true, 100)};
    return test_client_server::run(tests);
}

SEASTAR_TEST_CASE(test_http_client_server_full) {
    std::vector<std::tuple<bool, size_t>> tests = {
        std::make_tuple(true, 100),
        std::make_tuple(true, 10000),
        std::make_tuple(true, 100),
        std::make_tuple(true, 0),
        std::make_tuple(true, 5000),
        std::make_tuple(true, 10000),
        std::make_tuple(true, 9000),
        std::make_tuple(true, 10000)};
    return test_client_server::run(tests);
}

/*
 * return string in the given size
 * The string size takes the quotes into consideration.
 */
std::string get_value(int size) {
    std::stringstream res;
    for (auto i = 0; i < size - 2; i++) {
        res << "a";
    }
    return res.str();
}

/*
 * A helper object that map to a big json string
 * in the format of:
 * {"valu": "aaa....aa", "valu": "aaa....aa", "valu": "aaa....aa"...}
 *
 * The object can have an arbitrary size in multiplication of 10000 bytes
 *  */
struct extra_big_object : public json::json_base {
    json::json_element<sstring>* value;
    extra_big_object(size_t size) {
        value = new json::json_element<sstring>;
        // size = brackets + (name + ": " + get_value) * n + ", " * (n-1)
        // size = 2 + (name + 6 + get_value) * n - 2
        value->_name = "valu";
        *value = get_value(9990);
        for (size_t i = 0; i < size/10000; i++) {
            _elements.emplace_back(value);
        }
    }

    virtual ~extra_big_object() {
        delete value;
    }

    extra_big_object(const extra_big_object& o) {
        value = new json::json_element<sstring>;
        value->_name = o.value->_name;
        *value = (*o.value)();
        for (size_t i = 0; i < o._elements.size(); i++) {
            _elements.emplace_back(value);
        }
    }
};

SEASTAR_TEST_CASE(json_stream) {
    std::vector<extra_big_object> vec;
    size_t num_objects = 1000;
    size_t total_size = num_objects * 1000001 + 1;
    for (size_t i = 0; i < num_objects; i++) {
        vec.emplace_back(1000000);
    }
    return test_client_server::run_test(json::stream_object(vec), [total_size](size_t s, http_consumer& h) {
        BOOST_REQUIRE_EQUAL(h._size, total_size);
        return false;
    });
}

class json_test_handler : public handler_base {
    std::function<future<>(output_stream<char> &&)> _write_func;
public:
    json_test_handler(std::function<future<>(output_stream<char> &&)>&& write_func) : _write_func(write_func) {
    }
    future<std::unique_ptr<http::reply>> handle(const sstring& path,
            std::unique_ptr<http::request> req, std::unique_ptr<http::reply> rep) override {
        rep->write_body("json", _write_func);
        return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
    }
};

SEASTAR_TEST_CASE(content_length_limit) {
    return seastar::async([] {
        loopback_connection_factory lcf(1);
        http_server server("test");
        server.set_content_length_limit(11);
        loopback_socket_impl lsi(lcf);
        httpd::http_server_tester::listeners(server).emplace_back(lcf.get_server_socket());

        future<> client = seastar::async([&lsi] {
            connected_socket c_socket = lsi.connect(socket_address(ipv4_addr()), socket_address(ipv4_addr())).get0();
            input_stream<char> input(c_socket.input());
            output_stream<char> output(c_socket.output());

            output.write(sstring("GET /test HTTP/1.1\r\nHost: test\r\n\r\n")).get();
            output.flush().get();
            auto resp = input.read().get0();
            BOOST_REQUIRE_NE(std::string(resp.get(), resp.size()).find("200 OK"), std::string::npos);

            output.write(sstring("GET /test HTTP/1.1\r\nHost: test\r\nContent-Length: 11\r\n\r\nxxxxxxxxxxx")).get();
            output.flush().get();
            resp = input.read().get0();
            BOOST_REQUIRE_NE(std::string(resp.get(), resp.size()).find("200 OK"), std::string::npos);

            output.write(sstring("GET /test HTTP/1.1\r\nHost: test\r\nContent-Length: 17\r\n\r\nxxxxxxxxxxxxxxxx")).get();
            output.flush().get();
            resp = input.read().get0();
            BOOST_REQUIRE_EQUAL(std::string(resp.get(), resp.size()).find("200 OK"), std::string::npos);
            BOOST_REQUIRE_NE(std::string(resp.get(), resp.size()).find("413 Payload Too Large"), std::string::npos);

            input.close().get();
            output.close().get();
        });

        auto handler = new json_test_handler(json::stream_object("hello"));
        server._routes.put(GET, "/test", handler);
        server.do_accepts(0).get();

        client.get();
        server.stop().get();
    });
}

SEASTAR_TEST_CASE(test_100_continue) {
    return seastar::async([] {
        loopback_connection_factory lcf(1);
        http_server server("test");
        server.set_content_length_limit(11);
        loopback_socket_impl lsi(lcf);
        httpd::http_server_tester::listeners(server).emplace_back(lcf.get_server_socket());
        future<> client = seastar::async([&lsi] {
            connected_socket c_socket = lsi.connect(socket_address(ipv4_addr()), socket_address(ipv4_addr())).get0();
            input_stream<char> input(c_socket.input());
            output_stream<char> output(c_socket.output());

            for (auto version : {sstring("1.0"), sstring("1.1")}) {
                for (auto content : {sstring(""), sstring("xxxxxxxxxxx")}) {
                    for (auto expect : {sstring(""), sstring("Expect: 100-continue\r\n"), sstring("Expect: 100-cOnTInUE\r\n")}) {
                        auto content_len = content.empty() ? sstring("") : (sstring("Content-Length: ") + to_sstring(content.length()) + sstring("\r\n"));
                        sstring req = sstring("GET /test HTTP/") + version + sstring("\r\nHost: test\r\nConnection: Keep-Alive\r\n") + content_len + expect + sstring("\r\n");
                        output.write(req).get();
                        output.flush().get();
                        bool already_ok = false;
                        if (version == "1.1" && expect.length()) {
                            auto resp = input.read().get0();
                            BOOST_REQUIRE_NE(std::string(resp.get(), resp.size()).find("100 Continue"), std::string::npos);
                            already_ok = content.empty() && std::string(resp.get(), resp.size()).find("200 OK") != std::string::npos;
                        }
                        if (!already_ok) {
                            //If the body is empty, the final response might have already been read
                            output.write(content).get();
                            output.flush().get();
                            auto resp = input.read().get0();
                            BOOST_REQUIRE_EQUAL(std::string(resp.get(), resp.size()).find("100 Continue"), std::string::npos);
                            BOOST_REQUIRE_NE(std::string(resp.get(), resp.size()).find("200 OK"), std::string::npos);
                        }
                    }
                }
            }
            output.write(sstring("GET /test HTTP/1.1\r\nHost: test\r\nContent-Length: 17\r\nExpect: 100-continue\r\n\r\n")).get();
            output.flush().get();
            auto resp = input.read().get0();
            BOOST_REQUIRE_EQUAL(std::string(resp.get(), resp.size()).find("100 Continue"), std::string::npos);
            BOOST_REQUIRE_NE(std::string(resp.get(), resp.size()).find("413 Payload Too Large"), std::string::npos);

            input.close().get();
            output.close().get();
        });

        auto handler = new json_test_handler(json::stream_object("hello"));
        server._routes.put(GET, "/test", handler);
        server.do_accepts(0).get();

        client.get();
        server.stop().get();
    });
}


SEASTAR_TEST_CASE(test_unparsable_request) {
    // Test if a message that cannot be parsed as a http request is being replied with a 400 Bad Request response
    return seastar::async([] {
        loopback_connection_factory lcf(1);
        http_server server("test");
        loopback_socket_impl lsi(lcf);
        httpd::http_server_tester::listeners(server).emplace_back(lcf.get_server_socket());
        future<> client = seastar::async([&lsi] {
            connected_socket c_socket = lsi.connect(socket_address(ipv4_addr()), socket_address(ipv4_addr())).get0();
            input_stream<char> input(c_socket.input());
            output_stream<char> output(c_socket.output());

            output.write(sstring("GET /test HTTP/1.1\r\nhello\r\nContent-Length: 17\r\nExpect: 100-continue\r\n\r\n")).get();
            output.flush().get();
            auto resp = input.read().get0();
            BOOST_REQUIRE_NE(std::string(resp.get(), resp.size()).find("400 Bad Request"), std::string::npos);
            BOOST_REQUIRE_NE(std::string(resp.get(), resp.size()).find("Can't parse the request"), std::string::npos);

            input.close().get();
            output.close().get();
        });

        auto handler = new json_test_handler(json::stream_object("hello"));
        server._routes.put(GET, "/test", handler);
        server.do_accepts(0).get();

        client.get();
        server.stop().get();
    });
}

struct echo_handler : public handler_base {
    bool chunked_reply;
    echo_handler(bool chunked_reply_) : handler_base(), chunked_reply(chunked_reply_) {}

    future<std::unique_ptr<http::reply>> do_handle(std::unique_ptr<http::request>& req, std::unique_ptr<http::reply>& rep, sstring& content) {
        for (auto it : req->chunk_extensions) {
            content += it.first;
            if (it.second != "") {
                content += to_sstring("=") + it.second;
            }
        }
        for (auto it : req->trailing_headers) {
            content += it.first;
            if (it.second != "") {
                content += to_sstring(": ") + it.second;
            }
        }
        if (!chunked_reply) {
            rep->write_body("txt", content);
        } else {
            rep->write_body("txt", [ c = content ] (output_stream<char>&& out) {
                return do_with(std::move(out), [ c = std::move(c) ] (output_stream<char>& out) {
                    return out.write(std::move(c)).then([&out] {
                        return out.flush().then([&out] {
                            return out.close();
                        });
                    });
                });
            });
        }
        return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
    }
};

/*
 * A request handler that responds with the same body that was used in the request using the requests content_stream
 *  */
struct echo_stream_handler : public echo_handler {
    echo_stream_handler(bool chunked_reply = false) : echo_handler(chunked_reply) {}
    future<std::unique_ptr<http::reply>> handle(const sstring& path,
            std::unique_ptr<http::request> req, std::unique_ptr<http::reply> rep) override {
        return do_with(std::move(req), std::move(rep), sstring(), [this] (std::unique_ptr<http::request>& req, std::unique_ptr<http::reply>& rep, sstring& rep_content) {
            return do_until([&req] { return req->content_stream->eof(); }, [&req, &rep_content] {
                return req->content_stream->read().then([&rep_content] (temporary_buffer<char> tmp) {
                    rep_content += to_sstring(std::move(tmp));
                });
            }).then([&req, &rep, &rep_content, this] {
                return this->do_handle(req, rep, rep_content);
            });
        });
    }
};

/*
 * Same handler as above, but without using streams
 *  */
struct echo_string_handler : public echo_handler {
    echo_string_handler(bool chunked_reply = false) : echo_handler(chunked_reply) {}
    future<std::unique_ptr<http::reply>> handle(const sstring& path,
            std::unique_ptr<http::request> req, std::unique_ptr<http::reply> rep) override {
        return this->do_handle(req, rep, req->content);
    }
};

/*
 * Checks if the server responds to the request equivalent to the concatenation of all req_parts with a reply containing
 * the resp_parts strings, assuming that the content streaming is set to stream and the /test route is handled by handl
 * */
future<> check_http_reply (std::vector<sstring>&& req_parts, std::vector<std::string>&& resp_parts, bool stream, handler_base* handl) {
    return seastar::async([req_parts = std::move(req_parts), resp_parts = std::move(resp_parts), stream, handl] {
        loopback_connection_factory lcf(1);
        http_server server("test");
        server.set_content_streaming(stream);
        loopback_socket_impl lsi(lcf);
        httpd::http_server_tester::listeners(server).emplace_back(lcf.get_server_socket());
        future<> client = seastar::async([req_parts = std::move(req_parts), resp_parts = std::move(resp_parts), &lsi] {
            connected_socket c_socket = lsi.connect(socket_address(ipv4_addr()), socket_address(ipv4_addr())).get0();
            input_stream<char> input(c_socket.input());
            output_stream<char> output(c_socket.output());

            for (auto& str : req_parts) {
                output.write(std::move(str)).get();
                output.flush().get();
            }
            auto resp = input.read().get0();
            for (auto& str : resp_parts) {
                BOOST_REQUIRE_NE(std::string(resp.get(), resp.size()).find(std::move(str)), std::string::npos);
            }

            input.close().get();
            output.close().get();
        });

        server._routes.put(GET, "/test", handl);
        server.do_accepts(0).get();

        client.get();
        server.stop().get();
    });
};

static future<> test_basic_content(bool streamed, bool chunked_reply) {
    return seastar::async([streamed, chunked_reply] {
        loopback_connection_factory lcf(1);
        http_server server("test");
        if (streamed) {
            server.set_content_streaming(true);
        }
        httpd::http_server_tester::listeners(server).emplace_back(lcf.get_server_socket());
        future<> client = seastar::async([&lcf, chunked_reply] {
            class connection_factory : public http::experimental::connection_factory {
                loopback_socket_impl lsi;
            public:
                explicit connection_factory(loopback_connection_factory& f) : lsi(f) {}
                virtual future<connected_socket> make() override {
                    return lsi.connect(socket_address(ipv4_addr()), socket_address(ipv4_addr()));
                }
            };
            auto cln = http::experimental::client(std::make_unique<connection_factory>(lcf));

            {
                fmt::print("Simple request test\n");
                auto req = http::request::make("GET", "test", "/test");
                cln.make_request(std::move(req), [&] (const http::reply& resp, input_stream<char>&& in) {
                    if (chunked_reply) {
                        // need to drop empty chunk
                        return seastar::async([in = std::move(in)] () mutable {
                            util::skip_entire_stream(in).get();
                        });
                    }
                    return make_ready_future<>();
                }).get();
                // in fact, this case is to make sure that _next_ cases won't collect
                // garbage from the client connection
            }

            {
                fmt::print("Request with body test\n");
                auto req = http::request::make("GET", "test", "/test");
                req.write_body("txt", sstring("12345 78901\t34521345"));
                cln.make_request(std::move(req), [&] (const http::reply& resp, input_stream<char>&& in) {
                    BOOST_REQUIRE_EQUAL(resp._status, http::reply::status_type::ok);
                    if (!chunked_reply) {
                        BOOST_REQUIRE_EQUAL(resp.content_length, 20);
                    }
                    return seastar::async([in = std::move(in)] () mutable {
                        sstring body = util::read_entire_stream_contiguous(in).get0();
                        BOOST_REQUIRE_EQUAL(body, sstring("12345 78901\t34521345"));
                    });
                }).get();
            }

            {
                fmt::print("Request with content-length body\n");
                auto req = http::request::make("GET", "test", "/test");
                req.write_body("txt", 12, [] (output_stream<char>&& out) {
                    return seastar::async([out = std::move(out)] () mutable {
                        out.write(sstring("1234567890")).get();
                        out.write(sstring("AB")).get();
                        out.flush().get();
                        out.close().get();
                    });
                });
                cln.make_request(std::move(req), [&] (const http::reply& resp, input_stream<char>&& in) {
                    if (!chunked_reply) {
                        BOOST_REQUIRE_EQUAL(resp.content_length, 12);
                    }
                    return seastar::async([in = std::move(in)] () mutable {
                        sstring body = util::read_entire_stream_contiguous(in).get0();
                        BOOST_REQUIRE_EQUAL(body, sstring("1234567890AB"));
                    });
                }).get();
            }

            {
                const size_t size = 128*1024;
                fmt::print("Request with {}-kbytes content-length body\n", size >> 10);
                temporary_buffer<char> jumbo(size);
                temporary_buffer<char> jumbo_copy(size);
                for (size_t i = 0; i < size; i++) {
                    jumbo.get_write()[i] = 'a' + i % ('z' - 'a');
                    jumbo_copy.get_write()[i] = jumbo[i];
                }
                auto req = http::request::make("GET", "test", "/test");
                req.write_body("txt", size, [jumbo = std::move(jumbo)] (output_stream<char>&& out) mutable {
                    return seastar::async([out = std::move(out), jumbo = std::move(jumbo)] () mutable {
                        out.write(jumbo.get(), jumbo.size()).get();
                        out.flush().get();
                        out.close().get();
                    });
                });
                cln.make_request(std::move(req), [chunked_reply, size, jumbo_copy = std::move(jumbo_copy)] (const http::reply& resp, input_stream<char>&& in) mutable {
                    if (!chunked_reply) {
                        BOOST_REQUIRE_EQUAL(resp.content_length, size);
                    }
                    return seastar::async([in = std::move(in), jumbo_copy = std::move(jumbo_copy)] () mutable {
                        sstring body = util::read_entire_stream_contiguous(in).get0();
                        BOOST_REQUIRE_EQUAL(body, to_sstring(std::move(jumbo_copy)));
                    });
                }).get();
            }

            {
                fmt::print("Request with chunked body\n");
                auto req = http::request::make("GET", "test", "/test");
                req.write_body("txt", [] (auto&& out) -> future<> {
                    return seastar::async([out = std::move(out)] () mutable {
                        out.write(sstring("req")).get();
                        out.write(sstring("1234\r\n7890")).get();
                        out.flush().get();
                        out.close().get();
                    });
                });
                cln.make_request(std::move(req), [&] (const http::reply& resp, input_stream<char>&& in) {
                    BOOST_REQUIRE_EQUAL(resp._status, http::reply::status_type::ok);
                    if (!chunked_reply) {
                        BOOST_REQUIRE_EQUAL(resp.content_length, 13);
                    }
                    return seastar::async([in = std::move(in)] () mutable {
                        sstring body = util::read_entire_stream_contiguous(in).get0();
                        BOOST_REQUIRE_EQUAL(body, sstring("req1234\r\n7890"));
                    });
                }).get0();
            }

            {
                fmt::print("Request with expect-continue\n");
                auto req = http::request::make("GET", "test", "/test");
                req.write_body("txt", sstring("foobar"));
                req.set_expects_continue();
                cln.make_request(std::move(req), [&] (const http::reply& resp, input_stream<char>&& in) {
                    BOOST_REQUIRE_EQUAL(resp._status, http::reply::status_type::ok);
                    if (!chunked_reply) {
                        BOOST_REQUIRE_EQUAL(resp.content_length, 6);
                    }
                    return seastar::async([in = std::move(in)] () mutable {
                        sstring body = util::read_entire_stream_contiguous(in).get0();
                        BOOST_REQUIRE_EQUAL(body, sstring("foobar"));
                    });
                }).get();
            }

            {
                fmt::print("Request with incomplete content-length body\n");
                auto req = http::request::make("GET", "test", "/test");
                req.write_body("txt", 12, [] (output_stream<char>&& out) {
                    return seastar::async([out = std::move(out)] () mutable {
                        out.write(sstring("1234567890A")).get();
                        out.flush().get();
                        out.close().get();
                    });
                });
                BOOST_REQUIRE_THROW(cln.make_request(std::move(req), [] (const auto& resp, auto&& in) {
                    BOOST_REQUIRE(false); // should throw before handling response
                    return make_ready_future<>();
                }).get0(), std::runtime_error);
            }

            {
                bool callback_completed = false;
                fmt::print("Request with too large content-length body\n");
                auto req = http::request::make("GET", "test", "/test");
                req.write_body("txt", 12, [&callback_completed] (output_stream<char>&& out) {
                    return seastar::async([out = std::move(out), &callback_completed] () mutable {
                        out.write(sstring("1234567890ABC")).get();
                        out.flush().get();
                        out.close().get();
                        callback_completed = true;
                    });
                });
                BOOST_REQUIRE_NE(callback_completed, true); // should throw early
                BOOST_REQUIRE_THROW(cln.make_request(std::move(req), [] (const auto& resp, auto&& in) {
                    BOOST_REQUIRE(false); // should throw before handling response
                    return make_ready_future<>();
                }).get0(), std::runtime_error);
            }

            cln.close().get();
        });

        handler_base* handler;
        if (streamed) {
            handler = new echo_stream_handler(chunked_reply);
        } else {
            handler = new echo_string_handler(chunked_reply);
        }
        server._routes.put(GET, "/test", handler);
        server.do_accepts(0).get();

        client.get();
        server.stop().get();
    });
}

SEASTAR_TEST_CASE(test_string_content) {
    return test_basic_content(false, false);
}

SEASTAR_TEST_CASE(test_string_content_chunked) {
    return test_basic_content(false, true);
}

SEASTAR_TEST_CASE(test_stream_content) {
    return test_basic_content(true, false);
}

SEASTAR_TEST_CASE(test_stream_content_chunked) {
    return test_basic_content(true, true);
}

SEASTAR_TEST_CASE(test_not_implemented_encoding) {
    return check_http_reply({
        "GET /test HTTP/1.1\r\nHost: test\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
        "a\r\n1234567890\r\n",
        "a\r\n1234521345\r\n",
        "0\r\n\r\n"
    }, {"501 Not Implemented", "Encodings other than \"chunked\" are not implemented (received encoding: \"gzip, chunked\")"}, false, new echo_string_handler());
}

SEASTAR_TEST_CASE(test_full_chunk_format) {
    return check_http_reply({
        "GET /test HTTP/1.1\r\nHost: test\r\nTransfer-Encoding: chunked\r\n\r\n",
        "a;abc-def;hello=world;aaaa\r\n1234567890\r\n",
        "a;a0-!#$%&'*+.^_`|~=\"quoted string obstext\x80\x81\xff quoted_pair: \\a\"\r\n1234521345\r\n",
        "0\r\na:b\r\n~|`_^.+*'&%$#!-0a:  ~!@#$%^&*()_+\x80\x81\xff\r\n  obs fold  \r\n\r\n"
    }, {"12345678901234521345", "abc-def", "hello=world", "aaaa", "a0-!#$%&'*+.^_`|~=quoted string obstext\x80\x81\xff quoted_pair: a",
        "a: b", "~|`_^.+*'&%$#!-0a: ~!@#$%^&*()_+\x80\x81\xff obs fold"
    }, false, new echo_string_handler());
}

SEASTAR_TEST_CASE(test_chunk_extension_parser_fail) {
    return check_http_reply({
        "GET /test HTTP/1.1\r\nHost: test\r\nTransfer-Encoding: chunked\r\n\r\n",
        "7; \r\nnoparse\r\n",
        "0\r\n\r\n"
    }, {"400 Bad Request", "Can't parse chunk size and extensions"}, false, new echo_string_handler());
}

SEASTAR_TEST_CASE(test_trailer_part_parser_fail) {
    return check_http_reply({
        "GET /test HTTP/1.1\r\nHost: test\r\nTransfer-Encoding: chunked\r\n\r\n",
        "8\r\nparsable\r\n",
        "0\r\ngood:header\r\nbad=header\r\n\r\n"
    }, {"400 Bad Request", "Can't parse chunked request trailer"}, false, new echo_string_handler());
}

SEASTAR_TEST_CASE(test_too_long_chunk) {
    return check_http_reply({
        "GET /test HTTP/1.1\r\nHost: test\r\nTransfer-Encoding: chunked\r\n\r\n",
        "a\r\n1234567890\r\n",
        "a\r\n1234521345X\r\n",
        "0\r\n\r\n"
    }, {"400 Bad Request", "The actual chunk length exceeds the specified length"}, true, new echo_stream_handler());
}

SEASTAR_TEST_CASE(test_bad_chunk_length) {
    return check_http_reply({
        "GET /test HTTP/1.1\r\nHost: test\r\nTransfer-Encoding: chunked\r\n\r\n",
        "a\r\n1234567890\r\n",
        "aX\r\n1234521345\r\n",
        "0\r\n\r\n"
    }, {"400 Bad Request", "Can't parse chunk size and extensions"}, true, new echo_stream_handler());
}

SEASTAR_TEST_CASE(case_insensitive_header) {
    std::unique_ptr<seastar::http::request> req = std::make_unique<seastar::http::request>();
    req->_headers["conTEnt-LengtH"] = "17";
    BOOST_REQUIRE_EQUAL(req->get_header("content-length"), "17");
    BOOST_REQUIRE_EQUAL(req->get_header("Content-Length"), "17");
    BOOST_REQUIRE_EQUAL(req->get_header("cOnTeNT-lEnGTh"), "17");
    return make_ready_future<>();
}

SEASTAR_THREAD_TEST_CASE(multiple_connections) {
    loopback_connection_factory lcf(1);
    http_server server("test");
    httpd::http_server_tester::listeners(server).emplace_back(lcf.get_server_socket());
    socket_address addr{ipv4_addr()};

    std::vector<connected_socket> socks;
    // Make sure one shard has two connections pending.
    for (unsigned i = 0; i <= smp::count; ++i) {
        socks.push_back(loopback_socket_impl(lcf).connect(addr, addr).get0());
    }

    server.do_accepts(0).get();
    server.stop().get();
    lcf.destroy_all_shards().get();
}

SEASTAR_TEST_CASE(http_parse_response_status) {
    http_response_parser parser;
    parser.init();
    char r101[] = "HTTP/1.1 101 Switching Protocols\r\n\r\n";
    char r200[] = "HTTP/1.1 200 OK\r\nHost: localhost\r\nhello\r\n";

    parser.parse(r101, r101 + sizeof(r101), r101 + sizeof(r101));
    auto response = parser.get_parsed_response();
    BOOST_REQUIRE_EQUAL(response->_status_code, 101);

    parser.init();
    parser.parse(r200, r200 + sizeof(r200), r200 + sizeof(r200));
    response = parser.get_parsed_response();
    BOOST_REQUIRE_EQUAL(response->_status_code, 200);
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_shared_future) {
    shared_promise<json::json_return_type> p;
    auto fut = p.get_shared_future();

    (void)yield().then([p = std::move(p)] () mutable {
        p.set_value(json::json_void());
    });

    return std::move(fut).discard_result();
}

SEASTAR_TEST_CASE(test_url_encode_decode) {
    sstring encoded, decoded;
    bool ok;

    sstring all_valid = "~abcdefghijklmnopqrstuvwhyz-ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789.";
    encoded = http::internal::url_encode(all_valid);
    ok = http::internal::url_decode(encoded, decoded);
    BOOST_REQUIRE_EQUAL(ok, true);
    BOOST_REQUIRE_EQUAL(decoded, all_valid);
    BOOST_REQUIRE_EQUAL(all_valid, encoded);

    sstring some_invalid = "a?/!@#$%^&*()[]=.\\ \tZ";
    encoded = http::internal::url_encode(some_invalid);
    ok = http::internal::url_decode(encoded, decoded);
    BOOST_REQUIRE_EQUAL(ok, true);
    BOOST_REQUIRE_EQUAL(decoded, some_invalid);
    for (size_t i = 0; i < encoded.length(); i++) {
        if (encoded[i] != '%' && encoded[i] != '+') {
            auto f = std::find(std::begin(all_valid), std::end(all_valid), encoded[i]);
            BOOST_REQUIRE_NE(f, std::end(all_valid));
        }
    }

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_url_param_encode_decode) {
    http::request to_send;
    to_send._url = "/foo/bar";
    to_send.query_parameters["a"] = "a+a*a";
    to_send.query_parameters["b"] = "b/b\%b";

    http::request to_recv;
    to_recv._url = to_send.format_url();
    sstring url = to_recv.parse_query_param();

    BOOST_REQUIRE_EQUAL(url, to_send._url);
    BOOST_REQUIRE_EQUAL(to_recv.query_parameters.size(), to_send.query_parameters.size());
    for (const auto& p : to_send.query_parameters) {
        auto it = to_recv.query_parameters.find(p.first);
        BOOST_REQUIRE(it != to_recv.query_parameters.end());
        BOOST_REQUIRE_EQUAL(it->second, p.second);
    }

    return make_ready_future<>();
}
