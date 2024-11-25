# Developing and using Seastar

## Configuring the project

There are multiple ways to configure Seastar and its dependencies.

### Use system-packages for most dependencies

See the instructions in [README.md](./README.md).

### Download and install all external dependencies in a project-specific location

- First pull the git submodules using `git submodule update --init --recursive`

- Use `cmake-cooking` to prepare a development environment with all dependencies.  This allows for reproducible development environments, but means that approximately 3 GiB of dependencies get installed to `build/_cooking_`:

```
./cooking.sh
```

- The same as above, and enable DPDK support:

```
./cooking.sh -- -DSeastar_DPDK=ON
```

- Use system packages for all dependencies except `dpdk`, which is provided by `cmake-cooking` (and not yet widely available via system package-managers):

```
./cooking.sh -i dpdk
```

- Use `cmake-cooking` for all dependencies except for Boost:

```
./cooking.sh -e Boost
```

- The same, but compile in "release" mode:

```
./cooking.sh -e Boost -t Release
```

## Using an IDE with CMake support

If you use `configure.py` or `cooking.sh` to to configure Seastar, then the easiest way to use an IDE (such as Qt Creator, or CLion) for development is to instruct the IDE, when it invokes CMake, to include the following option:

```
-DCMAKE_PREFIX_PATH=${source_dir}/build/_cooking/installed
```

where `${source_dir}` is the root of the Seastar source tree on your file-system.

This will allow the IDE to also index Seastar's dependencies.

## Building the project

```
cd $my_build_dir
ninja
```

If you used `configure.py` to configure Seastar, then the build directory will be `build/$mode`. For example, `build/release`.

If you use `cooking.sh`, then the build directory will just be `build`.

## Running tests

Make sure you are in the "build" directory.

- Run unit tests:

```
ninja test_unit
```

- Run all tests:

```
ninja test
```

- Build and run a specific test:

```
ninja test_unit_thread_run
```


## Building documentation

Make sure you are in the "build" directory.

- Build all documentation:

```
ninja docs
```

- Build the tutorial in HTML form:

```
ninja doc_tutorial_html
```

- Build the tutorial in HTML form (one file per chapter):

```
ninja doc_tutorial_html_split
```

- Build the Doxygen documentation:

```
ninja doc_api
```

## Installing the project

Choose the install path:

With `configure.py`:

```
./configure.py --mode=release --prefix=/my/install/path
```

With `cooking.sh`:

```
./cooking.sh -- -DCMAKE_INSTALL_PREFIX=/my/install/path
```

```
ninja -C build install
```

## Using Seastar in an application

### CMake

Once Seastar has been installed, it is sufficient to add a dependency on Seastar with

```
find_package (Seastar ${VERSION} REQUIRED)

add_executable (my_program
  my_program.cc)

target_link_libraries (my_program
  PRIVATE Seastar::seastar)
```

where `VERSION` is the desired version.

If you'd like to use `cmake-cooking` to set up a development environment which includes Seastar and its dependencies (a "recipe"), you can include Seastar as follows:

```
cooking_ingredient (Seastar
  COOKING_RECIPE <DEFAULT>
  COOKING_CMAKE_ARGS
    -DSeastar_APPS=OFF
    -DSeastar_DEMOS=OFF
    -DSeastar_DOCS=OFF
    -DSeastar_TESTING=OFF
  EXTERNAL_PROJECT_ARGS
    SOURCE_DIR ${MY_SEASTAR_SOURCE_DIR})
```

### pkg-config

Seastar includes a `seastar.pc` file. It can be used from both the
install and build directories.

Compiling a single file:
```
g++ foo.cc -o foo $(pkg-config --libs --cflags --static /path/to/seastar.pc)
```

Compiling multiple files:
```
# Compiling sources into object files
g++ -c $(pkg-config --cflags /path/to/seastar.pc) foo.cc -o foo.o
g++ -c $(pkg-config --cflags /path/to/seastar.pc) bar.cc -o bar.o

# Linking object files into an executable
g++ -o foo_bar foo.o bar.o $(pkg-config --libs --static /path/to/seastar.pc)
```

The `--static` flag is needed to include transitive (private) dependencies of `libseastar.a`.
