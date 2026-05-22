Seastar socket peer failure handling
------------------------------------
Due to the nature of Seastar & TCP/IP stack, there are cases where reading/writing can hang, in multiple cases
such as, when a client isn't sending any data, or in the case there is a network failure, that doesn't allow to any data to pass other than Syn.

In such cases reading from a socket might never return.

If we look at the next example, we have an infinite read for any amount of data, but in a case of a Denial of Server multiple connections, can cause high pressure on the system. 
The next example is a bad example of how a socket can hang indefinitely.
```cpp
seastar::future<seastar::stop_iteration> process_data(seastar::output_stream<char> &out, seastar::input_stream<char> &in) {
    auto buf = in.read();

    if(buf) {
        co_await out.write(std::move(buf));
        co_await out.flush();
        co_return seastar::stop_iteration::no;
    } else {
        co_return seastar::stop_iteration::yes;
    }
}

seastar::future<> process_connection(seastar::connected_socket &s, seastar::output_stream<char> &out, seastar::input_stream<char> &in) {
    co_await seastar::repeat(std::bind(&process_data, std::ref(out), std::ref(in)));
    co_await out.close();
    co_await in.close();
}

seastar::future<> handle_connection(seastar::connected_socket s) {
    auto out = s.output();
    auto in = s.input();

    co_await do_with(std::move(s), std::move(out), std::move(in), std::bind_front(&process_connection));
}
```
In this example, the next step after read, will never happen, and we'll never reach the "end" state.

A simple solution for this case might be to add a timeout on the read operation, in the case the client doesn't do any progress, for instance.
The next example is a lacking handling for stream closure, and can cause a segmentation fault.
```cpp
seastar::future<seastar::stop_iteration> process_data(seastar::output_stream<char> &out, seastar::input_stream<char> &in) {
    auto buf = co_await seastar::with_timeout(seastar::lowres_clock::now() + std::chrono::seconds(1), in.read());

    if(buf) {
        co_await out.write(std::move(buf));
        co_await out.flush();
        co_return seastar::stop_iteration::no;
    } else {
        co_return seastar::stop_iteration::yes;
    }
}

seastar::future<> process_connection(seastar::connected_socket &s, seastar::output_stream<char> &out, seastar::input_stream<char> &in) {
    co_await seastar::repeat(std::bind(&process_data, std::ref(out), std::ref(in)));
    // This may not be called ever due to the time_out_error
    co_await out.close();
    co_await in.close();
}

seastar::future<> handle_connection(seastar::connected_socket s) {
    auto out = s.output();
    auto in = s.input();

    co_await do_with(std::move(s), std::move(out), std::move(in), std::bind_front(&process_connection));
}
```
In this example, a timeout exception will be propogonated, but there is a problem with that, ```seastar::with_timeout```, doesn't cancel ```in.read()``` future, which
means, the future might run, and can cause a Segmentation Fault, due to the fact that the ```sesastar::input_stream<char>```, was already destructed.

In order to resolve such a case, we can add a unique handling for the case of timeout
```cpp
seastar::future<seastar::stop_iteration> process_data(seastar::output_stream<char>& out,
                                                      seastar::input_stream<char>& in) {
    auto buf = co_await seastar::with_timeout(seastar::lowres_clock::now() + std::chrono::seconds(1), in.read());

    if(buf) {
        co_await out.write(std::move(buf));
        co_await out.flush();
        co_return seastar::stop_iteration::no;
    } else {
        co_return seastar::stop_iteration::yes;
    }
}

seastar::future<> process_connection(seastar::connected_socket& s, seastar::output_stream<char>& out,
                                     seastar::input_stream<char>& in) {
    std::exception_ptr eptr = nullptr;

    try {
        co_await seastar::repeat(std::bind(&process_data, std::ref(out), std::ref(in)));
    } catch(...) {
        eptr = std::current_exception();
    }

    if(eptr) {
        s.shutdown_input();
        co_await s.wait_input_shutdown();
        std::rethrow_exception(eptr);
    }

    co_await out.close();
    co_await in.close();
}

seastar::future<> handle_connection(seastar::connected_socket s) {
    auto out = s.output();
    auto in = s.input();

    co_await do_with(std::move(s), std::move(out), std::move(in), std::bind_front(&process_connection));
}
```

