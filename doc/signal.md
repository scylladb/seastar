# Signals

Seastar provides an interface to handle signals natively and safely as asynchronous tasks.

## Default Signal Handlers

Seastar sets by default signal handlers for SIGINT/SIGTERM that call reactor::stop(). The reactor will then execute callbacks installed by reactor::at_exit().

You can disable this behavior, by setting `app_template::config::auto_handle_sigint_sigterm` to false. This flag is provided in the header [seastar/core/app-template.hh](../include/seastar/core/app-template..hh). Then, Seastar will not set signal handlers, and the default behavior of the Linux kernel will be preserved (terminate the program).

### Examples

```C++
#include <seastar/core/app-template.hh>

int main(int ac, char** av) {
    seastar::app_template::config cfg;
    cfg.auto_handle_sigint_sigterm = false;
    seastar::app_template app(std::move(cfg));

    return app.run(argc, argv, [] {
        std::cout << "SIGINT/SIGTERM will terminate the program\n";
    });
}
```

## Custom Signal Handler

In order to set a custom signal handler, Seastar provides a procedure called `seastar::handle_signal` in the header [seastar/core/signal.hh](../include/seastar/core/signal.hh). It registers a custom handler for the specified signal based on the configuration params.

The procedure must be called inside the `app.run()` lambda, otherwise it's UB.

### Examples

```C++
#include <seastar/core/app-template.hh>
#include <seastar/core/signal.hh>

int main(int argc, char** argv) {
    seastar::app_template app;
    return app.run(argc, argv, [] {
        seastar::handle_signal(SIGINT, [&] {
            std::cout << "caught sigint\n";
        }, true);
    });
}
```

- [tests/unit/signal_test.cc](../tests/unit/signal_test.cc)
- [apps/lib/stop_signal.hh](../apps/lib/stop_signal.hh)
