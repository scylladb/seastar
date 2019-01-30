## Building Seastar on CentOS

### Building seastar on CentOS 7

Installing required packages:
```
sudo ./install-dependencies.sh
./cooking.sh -r dev -i c-ares -i fmt -t Release
```

To compile Seastar explicitly using gcc 5, use:
```
CXX=/opt/scylladb/bin/g++ ./cooking.sh -i c-ares -i fmt -t Release
ninja-build -C build
```
