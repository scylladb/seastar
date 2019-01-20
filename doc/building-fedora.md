## Building Seastar on Fedora

### Building seastar on Fedora 21 and later

Installing required packages:
```
sudo ./install-dependencies.sh
```

You then need to run the following to create the build directory:
```
./configure.py --mode=release
```
Note it is enough to run this once, and you don't need to repeat it before
every build.

Then finally:
```
ninja-build -C build/release
```

In case there are compilation issues, especially like ```g++: internal compiler error: Killed (program cc1plus)``` try giving more memory to gcc, either by limiting the amount of threads ( -j1 ) and/or allowing at least 4g ram to your machine
