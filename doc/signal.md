# Signals

Seastar provides an interface to handle signals natively and safely as asynchronous tasks.

## Default Signal Handlers

By default, Seastar sets signal handlers for `SIGINT` and `SIGTERM` that call `reactor::stop()`. The reactor then executes callbacks installed by `reactor::at_exit()`.

You can disable this behavior by setting `app_template::config::auto_handle_sigint_sigterm` to `false`. This flag is provided in [seastar/core/app-template.hh](../include/seastar/core/app-template.hh). Seastar will then leave these signal handlers unset, preserving the default Linux behavior of terminating the program.

### Examples

```cpp
#include <seastar/core/app-template.hh>
#include <iostream>
#include <utility>

int main(int argc, char** argv) {
    seastar::app_template::config cfg;
    cfg.auto_handle_sigint_sigterm = false;
    seastar::app_template app(std::move(cfg));

    return app.run(argc, argv, [] {
        std::cout << "SIGINT/SIGTERM will terminate the program\n";
    });
}
```

## Custom Signal Handler

To set a custom signal handler, use `seastar::handle_signal` from [seastar/core/signal.hh](../include/seastar/core/signal.hh). It registers a custom handler for the specified signal.

The function must be called inside the `app.run()` lambda; calling it elsewhere causes undefined behavior.

### Examples

```cpp
#include <seastar/core/app-template.hh>
#include <seastar/core/signal.hh>
#include <iostream>

int main(int argc, char** argv) {
    seastar::app_template app;
    return app.run(argc, argv, [] {
        seastar::handle_signal(SIGINT, [] {
            std::cout << "caught sigint\n";
        }, true);
    });
}
```

- [tests/unit/signal_test.cc](../tests/unit/signal_test.cc)
- [apps/lib/stop_signal.hh](../apps/lib/stop_signal.hh)
