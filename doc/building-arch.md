## Building Seastar on Arch

Installing required packages:
```
sudo ./install-dependencies.sh
```

To compile Seastar use:
```
./cooking.sh -r dev -i c-ares -i fmt
ninja -C build
```
