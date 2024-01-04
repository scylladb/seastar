Seastar socket peer failure handling
------------------------------------
Due to the nature of Seastar & TCP/IP stack, there are cases where reading/writing can hang, in multiple cases
such as, when a client isn't sending any data, or in the case there is a network failure, that doesn't allow to any data to pass other than Syn.

In such cases reading from a socket might never return.

If we look at the next example, we have an infinite read for any amount of data, but in a case of a Denial of Server multiple connections, can cause high pressure on the system. 
```cpp
seastar::future<> handle_connection(seastar::connected_socket s) {
    auto out = s.output();
    auto in = s.input();

    return do_with(std::move(s), std::move(out), std::move(in),
            [] (auto& s, auto& out, auto& in) {
        return seastar::repeat([&out, &in] {
            return in.read().then([&out] (auto buf) {
                if (buf) {
                    return out.write(std::move(buf)).then([&out] {
                        return out.flush();
                    }).then([] {
                        return seastar::stop_iteration::no;
                    });
                } else {
                    return seastar::make_ready_future<seastar::stop_iteration>(
                            seastar::stop_iteration::yes);
                }
            });
        }).then([&out] {
            return out.close();
        });
    });
}
```
In this example, the next step after read, will never happen, and we'll never reach the "end" state.

A simple solution for this case might be to add a timeout on the read operation, in the case the client doesn't do any progress, for instance 
```cpp
seastar::future<> handle_connection(seastar::connected_socket s) {
    auto out = s.output();
    auto in = s.input();

    return do_with(std::move(s), std::move(out), std::move(in),
            [] (auto& s, auto& out, auto& in) {
        return seastar::repeat([&out, &in] {
            return seastar::with_timeout(seastar::lowres_clock::now() + std::chrono::seconds(1), in.read()).then([&out] (auto buf) {
                if (buf) {
                    return out.write(std::move(buf)).then([&out] {
                        return out.flush();
                    }).then([] {
                        return seastar::stop_iteration::no;
                    });
                } else {
                    return seastar::make_ready_future<seastar::stop_iteration>(
                            seastar::stop_iteration::yes);
                }
            });
        }).then([&out] {
            return out.close();
        });
    });
}
```
In this example, a timeout exception will be propogonated, but there is a problem with that, ```seastar::with_timeout```, doesn't cancel ```in.read()``` future, which
means, the future might run, and can cause a Segmentation Fault, due to the fact that the ```sesastar::input_stream<char>```, was already destructed.

In order to resolve such a case, we can add a unique handling for the case of timeout
```cpp
seastar::future<> handle_connection(seastar::connected_socket s) {
    auto out = s.output();
    auto in = s.input();

    return do_with(std::move(s), std::move(out), std::move(in),
            [] (auto& s, auto& out, auto& in) {
        return seastar::repeat([&out, &in] {
            return seastar::with_timeout(seastar::lowres_clock::now() + std::chrono::seconds(1), in.read()).then([&out] (auto buf) {
                if (buf) {
                    return out.write(std::move(buf)).then([&out] {
                        return out.flush();
                    }).then([] {
                        return seastar::stop_iteration::no;
                    });
                } else {
                    return seastar::make_ready_future<seastar::stop_iteration>(
                            seastar::stop_iteration::yes);
                }
            }).handle_exception_type([&s] (const seastar::timed_out_error &err) {
                // Will resolve the currently waiting futures, with an exception
                s.shutdown_input();
                // Allows to wait untill the inputs are actually closed
                return s.wait_input_shutdown();
            });
        }).then([&out] {
            return out.close();
        });
    });
}
```

