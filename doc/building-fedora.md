## Building Seastar on Fedora

### Building seastar on Fedora 21 and later

Installing required packages:
```
yum install gcc-c++ libaio-devel ninja-build ragel hwloc-devel numactl-devel libpciaccess-devel cryptopp-devel xen-devel boost-devel libxml2-devel xfsprogs-devel gnutls-devel lksctp-tools-devel libubsan libasan
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

### Building seastar on Fedora 20

Installing GCC 4.9 for gnu++1y:
* Beware that this installation will replace your current GCC version.
```
yum install fedora-release-rawhide
yum --enablerepo rawhide update gcc-c++
yum --enablerepo rawhide install libubsan libasan
```

Installing required packages:
```
yum install libaio-devel ninja-build ragel hwloc-devel numactl-devel libpciaccess-devel cryptopp-devel gnutls-devel
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
