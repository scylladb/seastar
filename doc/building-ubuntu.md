## Building Seastar on Ubuntu

### Building seastar on Ubuntu 14.04/15.10/16.04

```
git clone https://github.com/scylladb/seastar.git
cd seastar
git submodule update --init --recursive
```

Installing required packages:
```
sudo ./install-dependencies.sh
```

To compile Seastar explicitly using gcc 5, use:
```
./configure.py --compiler=g++-5
ninja
```
