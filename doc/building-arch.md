## Building Seastar on Arch

Installing required packages:
```
sudo ./install-dependencies.sh
```

To compile Seastar use:
```
./configure.py --mode=release
ninja -C build
```
