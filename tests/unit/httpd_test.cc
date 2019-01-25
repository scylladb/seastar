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
#include <seastar/core/future-util.hh>
#include <seastar/testing/test_case.hh>
#include "loopback_socket.hh"
#include <boost/algorithm/string.hpp>
#include <seastar/core/thread.hh>
#include <seastar/util/noncopyable_function.hh>
#include <seastar/http/json_path.hh>
#include <sstream>

using namespace seastar;
using namespace httpd;

class handl : public httpd::handler_base {
public:
    virtual future<std::unique_ptr<reply> > handle(const sstring& path,
            std::unique_ptr<request> req, std::unique_ptr<reply> rep) {
        rep->done("html");
        return make_ready_future<std::unique_ptr<reply>>(std::move(rep));
    }
};

SEASTAR_TEST_CASE(test_reply)
{
    reply r;
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
    request req;
    req._url = "/a?q=%23%24%23";
    sstring url = http_server::connection::set_query_param(req);
    BOOST_REQUIRE_EQUAL(url, "/a");
    BOOST_REQUIRE_EQUAL(req.get_query_param("q"), "#$#");
    req._url = "/a?a=%23%24%23&b=%22%26%22";
    http_server::connection::set_query_param(req);
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
    std::unique_ptr<request> req = std::make_unique<request>();
    std::unique_ptr<reply> rep = std::make_unique<reply>();

    auto f1 =
            route.handle("/api", std::move(req), std::move(rep)).then(
                    [] (std::unique_ptr<reply> rep) {
                        BOOST_REQUIRE_EQUAL((int )rep->_status, (int )reply::status_type::ok);
                    });
    req.reset(new request);
    rep.reset(new reply);

    auto f2 =
            route.handle("/", std::move(req), std::move(rep)).then(
                    [] (std::unique_ptr<reply> rep) {
                        BOOST_REQUIRE_EQUAL((int )rep->_status, (int )reply::status_type::ok);
                    });
    req.reset(new request);
    rep.reset(new reply);
    auto f3 =
            route.handle("/api/abc", std::move(req), std::move(rep)).then(
                    [] (std::unique_ptr<reply> rep) {
                    });
    req.reset(new request);
    rep.reset(new reply);
    auto f4 =
            route.handle("/ap", std::move(req), std::move(rep)).then(
                    [] (std::unique_ptr<reply> rep) {
                        BOOST_REQUIRE_EQUAL((int )rep->_status,
                                            (int )reply::status_type::not_found);
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

    auto f1 = route->handle("/my/path/value1/text", std::make_unique<request>(), std::make_unique<reply>()).then([res1, route] (auto f) {
        BOOST_REQUIRE_EQUAL(*res1, true);
    });

    auto f2 = route->handle("/my/path/value2/text1", std::make_unique<request>(), std::make_unique<reply>()).then([res2, route] (auto f) {
        BOOST_REQUIRE_EQUAL(*res2, true);
    });

    auto f3 = route->handle("/my/path/value3/text2/text3", std::make_unique<request>(), std::make_unique<reply>()).then([res3, route] (auto f) {
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
    std::unique_ptr<seastar::httpd::request> req = std::make_unique<seastar::httpd::request>();
    ss.str("");
    req->_headers["Host"] = "localhost";
    return do_with(output_stream<char>(cr.transform(std::move(req), "json", output_stream<char>(memory_data_sink(ss), 32000, true))),
            std::vector<sstring>(std::move(buffer_parts)), [&ss, &cr] (output_stream<char>& os, std::vector<sstring>& parts) {
        return do_for_each(parts, [&os](auto& p) {
            return os.write(p);
        }).then([&os, &ss] {
            return os.close();
        });
    });
}

SEASTAR_TEST_CASE(test_transformer) {
    return do_with(std::stringstream(), content_replace("json"), [] (std::stringstream& ss, content_replace& cr) {
        return do_with(output_stream<char>(cr.transform(std::make_unique<seastar::httpd::request>(), "html", output_stream<char>(memory_data_sink(ss), 32000, true))),
                [&ss] (output_stream<char>& os) {
            return os.write(sstring("hello-{{Protocol}}-xyz-{{Host}}")).then([&os] {
                return os.close();
            });
        }).then([&ss, &cr] () {
            BOOST_REQUIRE_EQUAL(ss.str(), "hello-{{Protocol}}-xyz-{{Host}}");
            return test_transformer_stream(ss, cr, {"hell", "o-{", "{Pro", "tocol}}-xyz-{{Ho", "st}}{{Pr"}).then([&ss, &cr] {
                BOOST_REQUIRE_EQUAL(ss.str(), "hello-http-xyz-localhost{{Pr");
                return test_transformer_stream(ss, cr, {"hell", "o-{{", "Pro", "tocol}}{{Protocol}}-{{Protoxyz-{{Ho", "st}}{{Pr"}).then([&ss, &cr] {
                    BOOST_REQUIRE_EQUAL(ss.str(), "hello-httphttp-{{Protoxyz-localhost{{Pr");
                    return test_transformer_stream(ss, cr, {"hell", "o-{{Pro", "t{{Protocol}}ocol}}", "{{Host}}"}).then([&ss, &cr] {
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
        return do_with(loopback_connection_factory(), foreign_ptr<shared_ptr<http_server>>(make_shared<http_server>("test")),
                [reader, &write_func] (loopback_connection_factory& lcf, auto& server) {
            return do_with(loopback_socket_impl(lcf), [&server, &lcf, reader, &write_func](loopback_socket_impl& lsi) {
                httpd::http_server_tester::listeners(*server).emplace_back(lcf.get_server_socket());

                auto client = seastar::async([&lsi, reader] {
                    connected_socket c_socket = std::get<connected_socket>(lsi.connect(socket_address(ipv4_addr()), socket_address(ipv4_addr())).get());
                    input_stream<char> input(std::move(c_socket.input()));
                    output_stream<char> output(std::move(c_socket.output()));
                    bool more = true;
                    size_t count = 0;
                    while (more) {
                        http_consumer htp;
                        htp._concat = false;

                        write_request(output).get();
                        repeat([&c_socket, &input, &htp] {
                            return input.read().then([&c_socket, &input, &htp](const temporary_buffer<char>& b) mutable {
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

                auto writer = seastar::async([&server, &write_func] {
                    class test_handler : public handler_base {
                        size_t count = 0;
                        http_server& _server;
                        std::function<future<>(output_stream<char> &&)> _write_func;
                        promise<> _all_message_sent;
                    public:
                        test_handler(http_server& server, std::function<future<>(output_stream<char> &&)>&& write_func) : _server(server), _write_func(write_func) {
                        }
                        future<std::unique_ptr<reply>> handle(const sstring& path,
                                std::unique_ptr<request> req, std::unique_ptr<reply> rep) override {
                            rep->write_body("json", std::move(_write_func));
                            count++;
                            _all_message_sent.set_value();
                            return make_ready_future<std::unique_ptr<reply>>(std::move(rep));
                        }
                        future<> wait_for_message() {
                            return _all_message_sent.get_future();
                        }
                    };
                    auto handler = new test_handler(*server, std::move(write_func));
                    server->_routes.put(GET, "/test", handler);
                    when_all(server->do_accepts(0), handler->wait_for_message()).get();
                });
                return when_all(std::move(client), std::move(writer));
            }).discard_result().then_wrapped([&server] (auto f) {
                f.ignore_ready_future();
                return server->stop();
            });
        });
    }
    static future<> run(std::vector<std::tuple<bool, size_t>> tests) {
        return do_with(loopback_connection_factory(), foreign_ptr<shared_ptr<http_server>>(make_shared<http_server>("test")),
                [tests] (loopback_connection_factory& lcf, auto& server) {
            return do_with(loopback_socket_impl(lcf), [&server, &lcf, tests](loopback_socket_impl& lsi) {
                httpd::http_server_tester::listeners(*server).emplace_back(lcf.get_server_socket());

                auto client = seastar::async([&lsi, tests] {
                    connected_socket c_socket = std::get<connected_socket>(lsi.connect(socket_address(ipv4_addr()), socket_address(ipv4_addr())).get());
                    input_stream<char> input(std::move(c_socket.input()));
                    output_stream<char> output(std::move(c_socket.output()));
                    bool more = true;
                    size_t count = 0;
                    while (more) {
                        http_consumer htp;
                        write_request(output).get();
                        repeat([&c_socket, &input, &htp] {
                            return input.read().then([&c_socket, &input, &htp](const temporary_buffer<char>& b) mutable {
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

                auto writer = seastar::async([&server, tests] {
                    class test_handler : public handler_base {
                        size_t count = 0;
                        http_server& _server;
                        std::vector<std::tuple<bool, size_t>> _tests;
                        promise<> _all_message_sent;
                    public:
                        test_handler(http_server& server, const std::vector<std::tuple<bool, size_t>>& tests) : _server(server), _tests(tests) {
                        }
                        future<std::unique_ptr<reply>> handle(const sstring& path,
                                std::unique_ptr<request> req, std::unique_ptr<reply> rep) override {
                            rep->write_body("txt", make_writer(std::get<size_t>(_tests[count]), std::get<bool>(_tests[count])));
                            count++;
                            if (count == _tests.size()) {
                                _all_message_sent.set_value();
                            }
                            return make_ready_future<std::unique_ptr<reply>>(std::move(rep));
                        }
                        future<> wait_for_message() {
                            return _all_message_sent.get_future();
                        }
                    };
                    auto handler = new test_handler(*server, tests);
                    server->_routes.put(GET, "/test", handler);
                    when_all(server->do_accepts(0), handler->wait_for_message()).get();
                });
                return when_all(std::move(client), std::move(writer));
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
                return repeat([&str, &remain, success] () mutable {
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

    extra_big_object(extra_big_object&&) = default;
};

SEASTAR_TEST_CASE(json_stream) {
    std::vector<extra_big_object> vec;
    size_t num_objects = 1000;
    size_t total_size = num_objects * 1000001 + 1;
    for (size_t i = 0; i < num_objects; i++) {
        vec.emplace_back(extra_big_object(1000000));
    }
    return test_client_server::run_test(json::stream_object(vec), [total_size](size_t s, http_consumer& h) {
        BOOST_REQUIRE_EQUAL(h._size, total_size);
        return false;
    });
}
