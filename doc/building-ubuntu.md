## Building Seastar on Ubuntu

### Building seastar on Ubuntu 14.04

Installing required packages:
```
sudo apt-get install libaio-dev ninja-build ragel libhwloc-dev libnuma-dev libpciaccess-dev libcrypto++-dev libboost-all-dev libxen-dev libxml2-dev xfslibs-dev
```

Installing GCC 4.9 for gnu++1y. Unlike the Fedora case above, this will
not harm the existing installation of GCC 4.8, and will install an
additional set of compilers, and additional commands named gcc-4.9,
g++-4.9, etc., that need to be used explicitly, while the "gcc", "g++",
etc., commands continue to point to the 4.8 versions.

```
# Install add-apt-repository
sudo apt-get install software-properties-common python-software-properties
# Use it to add Ubuntu's testing compiler repository
sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt-get update
# Install gcc 4.9 and relatives
sudo apt-get install g++-4.9
# Also set up necessary header file links and stuff (?)
sudo apt-get install gcc-4.9-multilib g++-4.9-multilib
```

To compile Seastar explicitly using gcc 4.9, use:
```
./configure.py --compiler=g++-4.9
ninja
```

