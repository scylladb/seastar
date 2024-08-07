# Signals

Seastar provides an interface to handle signals natively and safely as asynchronous tasks.

It provides a dedicated header called [seastar/core/signal.hh](../include/seastar/core/signal.hh).

## Usage

You should call `seastar::handle_signal` procedure in order to register the provided signal handler for the specified signal based on the configuration params.

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
