# Seastar Coding Style

## Files

Header files have the `.hh` extension, source files use the `.cc` extension. All files must have a license and copyright blurb. Use `#pragma once` instead of an include guard.

Header files which contain a public part of the interface of Seastar go in the `include` directory. Internal header and source files which are private to the implementation go in the `src` directory.

## Whitespace

Use spaces only; NEVER tabs. Rationale: tabs render differently on each system.

An _indent_ is four spaces. A double indent is eight spaces, a half-indent is two spaces.

## Naming

We follow the C++ and Boost naming conventions: class names, variables, functions, and concepts are `words_separated_by_whitespace`.

Private data members are prefixed by an underscore:

```c++
class my_class {
    int _a_member;
public:
    void foo() {
        _a_member = 3;
    }
};
```

Think of the leading underscore as a shorthand for `this->`.

Template parameters use `CamelCase`

Note: because the Concept Technical Specification used CamelCase for concepts,
some Seastar concepts alse use CamelCase. These will be gradually deprecated
and replaced with snake_case names. New concepts should use snake_case.

## Including header files

In any file, to include a public header file (one in the `include` directory), use an absolute path with `<>` like this:

```c++
#include <seastar/core/future.hh>
```

In any private file, to include a private header file (one in the `src` directory), use an absolute path with `""` like this:

```c++
#include "core/future_impl.hh"
```

Header files in Seastar must be self-contained, i.e., each can be included without having to include specific other headers first. To verify that your change did not break this property, run `ninja checkheaders` in the build directory.

## Braced blocks

All nested scopes are braced, even when the language allows omitting the braces (such as an if-statement), this makes patches simpler and is more consistent. The opening brace is merged with the line that opens the scope (class definition, function definition, if statement, etc.) and the body is indented.

```c++
void a_function() {
    if (some condition) {
        stmt;
    } else {
        stmt;
    }
}
```

An exception is namespaces -- the body is _not_ indented, to prevent files that are almost 100% whitespace left margin.

When making a change, if you need to insert an indentation level, you can temporarily break the rules by insering a half-indent, so that the patch is easily reviewable:

```c++
void a_function() {
  while (something) {   // new line - half indent
    if (some condition) {
        stmt;
    } else {
        stmt;
    }
  }                      // new line
}
```

A follow-up patch can restore the indents without any functional changes.

## Function parameters

Avoid output parameters; use return values instead.  In/out parameters are tricky, but in some cases they are relatively standard, such as serialization/deserialization.

If a function accepts a lambda or an `std::function`, make it the last argument, so that it can be easily provided inline:

```c++
template <typename Func>
void function_accepting_a_lambda(int a, int b, Func func);

int f() {
    return function_accepting_a_lambda(2, 3, [] (int x, int y) {
        return x + y;
    });
}
```

## Complex return types

If a function returns a complicated return type, put its return type on a separate line, otherwise it becomes hard to see where the return type ends and where the function name begins:

```c++
template <typename T1, T2>
template <typename T3, T4>
std::vector<typename a_struct<T1, T2>::some_nested_class<T3, T4>>  // I'm the return type
a_struct<T1, T2>::a_function(T3 a, T4 b) {                         // And I'm the function name
    // ...
}
```

## Whitespace around operators

Whitespace around operators should match their precedence: high precedence = no spaces, low precedency = add spaces:

```c++
     return *a + *b;  // good
     return * a+* b;  // bad
```

`if`, `while`, `return` (and `template`) are not function calls, so they get a space after the keyword.

## Long lines

If a line becomes excessively long (>160 characters?), or is just complicated, break it into two or more lines.  The second (and succeeding lines) are _continuation lines_, and have a double indent:

```c++
    if ((some_condition && some_other_condition)
            || (more complicated stuff here...)   // continuation line, double indent
            || (even more complicated stuff)) {   // another continuation line
        do_something();  // back to single indent
    }
```

Of course, long lines or complex conditions may indicate that refactoring is in order.

## Generic lambdas and types

Generic lambdas (`[] (auto param)`) are discouraged where the type is known. Generic
lambdas reduce the compiler's and other tools' ability to reason about the code.
In case the actual type of `param` doesn't match the programmers expectations,
the compiler will only detect an error in the lambda body, or perhaps
even lower down the stack if more generic functions are called. In the case of an
IDE, most of its functionality is disabled in a generic lambda, since it can't
assume anything about that parameter.

Of course, when there is a need to support multiple types, genericity is the correct
tool. Even then, type parameters should be constrained with concepts, in order to
catch type mismatches early rather than deep in the instantiation chain.


