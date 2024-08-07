# Signals

Seastar provides an interface to handle signals natively and safely as asynchronous tasks.

It provides a dedicated header called [seastar/core/signal.hh](../include/seastar/core/signal.hh).

## Usage

You need to construct a `signal_handler` instance that will register the provided signal handler for the specified signal based on the configuration params.

At the moment, the signal handler will remain past object destruction.

### Examples

```C++
#include <seastar/core/signal.hh>

seastar::signal_handler sighup(SIGHUP, [&] {
    std::cout << "caught signal\n";
});
```

- [tests/unit/signal_test.cc](../tests/unit/signal_test.cc)
- [apps/lib/stop_signal.hh](../apps/lib/stop_signal.hh)
