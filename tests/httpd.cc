/*
 * Copyright 2015 Cloudius Systems
 */

#include "http/httpd.hh"
#include "http/handlers.hh"
#include "http/matcher.hh"
#include "http/matchrules.hh"
#include "json/formatter.hh"
#include "http/routes.hh"
#include "http/exception.hh"
#include "http/transformers.hh"
#include "core/future-util.hh"
#include "tests/test-utils.hh"
#include "loopback_socket.hh"
#include <boost/algorithm/string.hpp>
#include "core/thread.hh"

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
    BOOST_REQUIRE_EQUAL(m.match("/abc/hello", 4, param), 10);
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_param_matcher)
{

    param_matcher m("param");
    parameters param;
    BOOST_REQUIRE_EQUAL(m.match("/abc/hello", 4, param), 10);
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

SEASTAR_TEST_CASE(test_transformer) {
    request req;
    content_replace cr("json");
    sstring content = "hello-{{Protocol}}-xyz-{{Host}}";
    cr.transform(content, req, "html");
    BOOST_REQUIRE_EQUAL(content, "hello-{{Protocol}}-xyz-{{Host}}");
    req._headers["Host"] = "localhost";
    cr.transform(content, req, "json");
    BOOST_REQUIRE_EQUAL(content, "hello-http-xyz-localhost");
    return make_ready_future<>();
}

struct http_consumer {
    std::map<sstring, std::string> _headers;
    std::string _body;
    uint32_t _remain = 0;
    std::string _current;
    char last = '\0';

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
                        _body = _body + last;
                        _remain--;
                        if (_remain <= 1 && status == status_type::READING_BODY_BY_SIZE) {
                            _body = _body + c;
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

    static std::function<future<>(output_stream<char>&& o_stream)> make_writer(size_t len, bool success) {
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

