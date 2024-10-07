# Testing

Seastar leverages Boost.Test and provides facilities for developers to implement tests in coroutines.

## Test Declarations

There are three categories of boost-based tests in our system:

* Boost.Test Native: Tests defined using `BOOST_AUTO_TEST_CASE` and related macros from Boost.Test. For more information, see the [Boost Test documentation](https://www.boost.org/doc/libs/release/libs/test/doc/html/boost_test/utf_reference/test_org_reference.html).
* [Coroutine](https://github.com/scylladb/seastar/blob/master/doc/tutorial.md#coroutines): Tests defined using `SEASTAR_TEST_CASE`. The test body returns a future, allowing implementation as a coroutine.
* Coroutine with [`seastar::thread`](https://github.com/scylladb/seastar/blob/master/doc/tutorial.md#seastarthread): Tests defined using `SEASTAR_THREAD_TEST_CASE`. The test body is launched in a Seastar thread, allowing the use of Seastar coroutines. These tests should return `void`.

## Choosing the Appropriate Macro

* Use `SEASTAR_TEST_CASE` or `SEASTAR_THREAD_TEST_CASE` if you need to run tests in a Seastar reactor thread, typically for Seastar coroutines. `SEASTAR_THREAD_TEST_CASE` can be more convenient in some cases.
* For Seastar facilities that don't depend on coroutines, consider using `BOOST_AUTO_TEST_CASE`.


## Tests' organization

* Tests defined with `BOOST_AUTO_TEST_CASE` are driven by Boost.Test's built-in test runner. They require defining [`BOOST_TEST_MODULE`](https://www.boost.org/doc/libs/release/libs/test/doc/html/boost_test/utf_reference/link_references/link_boost_test_module_macro.html) to include the runner in the executable. These tests support additional features like fixtures and data-driven testing.
* `SEASTAR_TEST_CASE` and `SEASTAR_THREAD_TEST_CASE` tests only support decorators and share a common test runner, allowing them to be co-located in the same test suite.
* Seastar tests can be co-located with "native" Boost tests. The `SEASTAR_TEST_CASE` / `SEASTAR_THREAD_TEST_CASE` restrictions above apply to the entire test binary, and the `BOOST_TEST_MODULE` macro must not be defined.

## Adding Tests in `CMakeLists.txt`

For Seastar tests (and binaries that contain both Seastar and Boost tests):

```cmake
seastar_add_test(meow_test
  KIND SEASTAR
  SOURCES meow.cc)
```

or simply:

```cmake
seastar_add_test(meow_test
  SOURCES meow.cc)
```

The `KIND` parameter of `seastar_add_test()` function defaults to `SEASTAR`, using the Seastar test runner by defining the `SEASTAR_TESTING_MAIN` macro.

For "native" Boost(-only) tests:

```cmake
seastar_add_test(woof_test
  KIND BOOST
  SOURCE woof.cc)
```

