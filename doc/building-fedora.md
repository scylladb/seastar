## Building Seastar on Fedora

### Building seastar on Fedora 21 and later

Installing required packages:
```
sudo ./install-dependencies.sh
```

You then need to run the following to create the "build.ninja" file:
```
./configure.py
```
Note it is enough to run this once, and you don't need to repeat it before
every build. build.ninja includes a rule which will automatically re-run
./configure.py if it changes.

Then finally:
```
ninja-build
```

In case there are compilation issues, especially like ```g++: internal compiler error: Killed (program cc1plus)``` try giving more memory to gcc, either by limiting the amount of threads ( -j1 ) and/or allowing at least 4g ram to your machine
