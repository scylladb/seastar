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

> Promises 和 future很多的简化了异步编程,由于它们解耦了事件生产者与事件消费者之间; promise是事件生产者, 因为promise是用来提供future的对象或者函数,
> 而那些需要使用future就是事件的消费者, future是一个可能还没有完成的结果;

Consuming a future
------------------

You consume a future by using its *then()* method, providing it with a
callback (typically a lambda).  For example, consider the following
operation:

```C++
future<int> get();   // get()本身是一个promise,返回值是一个future,可以通过.then来消费future
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

> 如果*then()*的lambda表达式返回一个future,然后对这个future再进行*then()*来消费,这样得到的结果将于上面得到相同的值,这样的好处就是可以防止嵌套的
> lambda表达式;通过链式的方式来解决这个问题;

```C++
future<int> get();   // promises an int will be produced eventually
future<> put(int)    // promises to store an int

//get().then()会返回一个future,这个future其实是有put(int) 产生的future,然后再次消费,与上面的直接消费put的future是一样的;
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

> 当上面的循环开始运行的时候, 上面的两个then是立马会被执行到的(表示上面的loop_to会立即会被执行完,不会被阻塞),
> 但是then的函数体并没有马上执行,在等待一些条件的触发;

1. `get()` is called, initiates the I/O operation, and allocates a
   temporary structure (call it `f1`).
2. The first `then()` call chains its body to `f1` and allocates
   another temporary structure, `f2`.
3. The second `then()` call chains its body to `f2`.

> 1. `get()` 被调用的时候,会开始一个I/O操作(这个指的是get函数体里面具体要做的事情),并且会分配一个临时的结构体(称为 f1)
> 2. 第一个`then()`被调用的时候(这里所谓的调用:其实就是执行到这个关键字的时候), 它会把`then()`的函数体放到f1中并且再一次分配内存生成一个临时的结构叫做 f2
> 3. 第二个`then()`被调用的时候做了和步骤2类似的操作,把自己的`then()`的函数体放到f2中;

Again, all this runs immediately without waiting for anything.

> 所以上面的操作都会立即完成而不会等待任何事情

After the I/O operation initiated by `get()` completes, it calls the
continuation stored in `f1`, calls it, and frees `f1`.  The continuation
calls `put()`, which initiates the I/O operation required to perform
the store, and allocates a temporary object `f12`, and chains some glue
code to it.

> 当`get()`的I/O操作完成以后,它会去调用存放在f1中代码,然后释放f1这个临时结构,
> 在调用f1的时候,返现了要调用`put()`,于是与`get()`类似,会创建一个临时的结构体`f12`,做一些连接性的代码连接之后的操作;

After the I/O operation initiated by `put()` completes, it calls the
continuation associated with `f12`, which simply tells it to call the
continuation associated with `f2`.  This continuation simply calls
`loop_to()`.  Both `f12` and `f2` are freed. `loop_to()` then calls
`get()`, which starts the process all over again, allocating new versions
of `f1` and `f2`.

> 当`put()`的真实操作完成之后,它会去调用f12,而f12的操作很简单,它仅仅只是调用了与之相连的f2,
> 而f2本身也很简单的调用了`loop_to()`. 当现在为止f12和f2都已经被释放了;`loop_to`之后会继续调用`get()`,
> 之后的过程和之前的所有过程都是一样的;

> 自己的想法: 每一个次调用promise的时候会有上面的过程,比如 启动函数本身要执行的内容 + 分配一个空间用来存放后面的操作,如果没有`then`的
> 就做一些连接代码操作,主要就与返回之后的代码链接到一起;比如上面的最后的`loop_to`,它本身还要进行循环操作,但是它依然还是会分配一个内存空间
> 这个内存空间存放到是第一次调用`loop_to`的分配内存的地方; 比如:

``` C++
loop_to(5).then([](){
    std::cout << "end" << std::endl;
});
```
> 这个例子中,在调用loop_to(5) 这个promise的时候会分配临时结构体f1,用来存放then的函数体,loop_to内存执行到最后的loop_to依然还是会分配一个内存叫做f2,
> f2中存放的就是要去调用f1这块内容,这可能就是return本身的效果;连接后面需要调用的函数

Handling exceptions
-------------------

If a `.then()` clause throws an exception, the scheduler will catch it
and cancel any dependent `.then()` clauses.  If you want to trap the
exception, add a `.then_wrapped()` clause at the end:

> 如果要处理异常可以在你的处理流程最后加上`then_wrapped()`, 它会重新抛出异常,所以你可以在then_wrapped中加入异常处理函数
> then_wrapped也是每一次都会被调用的,调用到了then_wrapped中不表示你一定出了异常,只要到了异常处理模块中才表示你处理异常;
> 从源码的角度上,then 与 then_wrapped区别在于then会对result判断是否为exception,而then_wrapped会将上面传入的参数重新包装成一个future传给then_wrapped的lambda表达式，当lambda表达式中调用get就会重新抛出异常;

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
        // 获得f future里面的值,这个时候如果是异常就会重新抛出异常
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

> 异常会接收到f参数,调用f.get函数,会重新抛出异常,这个时候就可以处理异常了

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

future<my_type> my_future();

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

