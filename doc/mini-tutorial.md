Futures and promises
--------------------

A *future* is a result of a computation that may not be available yet.
Examples include:

  * a data buffer that we are reading from the network
  * the expiration of a timer
  * the completion of a disk write
  * the result computation that requires the values from
    one or more other futures.

a *promise* is an object or function that provides you with a future,
with the expectation that it will fulfill the future.

Promises and futures simplify asynchronous programming since they decouple
the event producer (the promise) and the event consumer (whoever uses the
future).  Whether the promise is fulfilled before the future is consumed,
or vice versa, does not change the outcome of the code.

Consuming a future
------------------

You consume a future by using its *then()* method, providing it with a
callback (typically a lambda).  For example, consider the following
operation:

```C++
future<int> get();   // promises an int will be produced eventually
future<> put(int)    // promises to store an int

void f() {
    get().then([] (int value) {
        put(value + 1).then([] {
            std::cout << "value stored successfully\n";
        });
    });
}
```

Here, we initiate a *get()* operation, requesting that when it completes, a
*put()* operation will be scheduled with an incremented value.  We also
request that when the *put()* completes, some text will be printed out.

Chaining futures
----------------

If a *then()* lambda returns a future (call it x), then that *then()*
will return a future (call it y) that will receive the same value.  This
removes the need for nesting lambda blocks; for example the code above
could be rewritten as:

```C++
future<int> get();   // promises an int will be produced eventually
future<> put(int)    // promises to store an int

void f() {
    get().then([] (int value) {
        return put(value + 1);
    }).then([] {
        std::cout << "value stored successfully\n";
    });
}
```

Loops
-----

Loops are achieved with a tail call; for example:

```C++
future<int> get();   // promises an int will be produced eventually
future<> put(int)    // promises to store an int

future<> loop_to(int end) {
    if (value == end) {
        return make_ready_future<>();
    }
    get().then([end] (int value) {
        return put(value + 1);
    }).then([end] {
        return loop_to(end);
    });
}
```

The *make_ready_future()* function returns a future that is already
available --- corresponding to the loop termination condition, where
no further I/O needs to take place.

Under the hood
--------------

When the loop above runs, both *then* method calls execute immediately
--- but without executing the bodies.  What happens is the following:

1. `get()` is called, initiates the I/O operation, and allocates a
   temporary structure (call it `f1`).
2. The first `then()` call chains its body to `f1` and allocates
   another temporary structure, `f2`.
3. The second `then()` call chains its body to `f2`.

Again, all this runs immediately without waiting for anything.

After the I/O operation initiated by `get()` completes, it calls the
continuation stored in `f1`, calls it, and frees `f1`.  The continuation
calls `put()`, which initiates the I/O operation required to perform
the store, and allocates a temporary object `f12`, and chains some glue
code to it.

After the I/O operation initiated by `put()` completes, it calls the
continuation associated with `f12`, which simply tells it to call the
continuation associated with `f2`.  This continuation simply calls
`loop_to()`.  Both `f12` and `f2` are freed. `loop_to()` then calls
`get()`, which starts the process all over again, allocating new versions
of `f1` and `f2`.

Handling exceptions
-------------------

If a `.then()` clause throws an exception, the scheduler will catch it
and cancel any dependent `.then()` clauses.  If you want to trap the
exception, add a `.then_wrapped()` clause at the end:

```C++
future<buffer> receive();
request parse(buffer buf);
future<response> process(request req);
future<> send(response resp);

void f() {
    receive().then([] (buffer buf) {
        return process(parse(std::move(buf));
    }).then([] (response resp) {
        return send(std::move(resp));
    }).then([] {
        f();
    }).then_wrapped([] (auto&& f) {
        try {
            f.get();
        } catch (std::exception& e) {
            // your handler goes here
        }
    });
}
```

The previous future is passed as a parameter to the lambda, and its value can
be inspected with `f.get()`. When the `get()` variable is called as a
function, it will re-throw the exception that aborted processing, and you can
then apply any needed error handling.  It is essentially a transformation of

```C++
buffer receive();
request parse(buffer buf);
response process(request req);
void send(response resp);

void f() {
    try {
        while (true) {
            auto req = parse(receive());
            auto resp = process(std::move(req));
            send(std::move(resp));
        }
    } catch (std::exception& e) {
        // your handler goes here
    }
}
```

Note, however, that the `.then_wrapped()` clause will be scheduled both when
exception occurs or not. Therefore, the mere fact that `.then_wrapped()` is
executed does not mean that an exception was thrown. Only the execution of the
catch block can guarantee that.


This is shown below:

```C++

future<my_type> receive();

void f() {
    receive().then_wrapped([] (future<my_type> f) {
        try {
            my_type x = f.get();
            return do_something(x);
        } catch (std::exception& e) {
            // your handler goes here
        }
    });
}
```
### Setup notes

SeaStar is a high performance framework and tuned to get the best
performance by default. As such, we're tuned towards polling vs interrupt
driven. Our assumption is that applications written for SeaStar will be
busy handling 100,000 IOPS and beyond. Polling means that each of our
cores will consume 100% cpu even when no work is given to it.

