## Building Seastar on CentOS

### Building seastar on CentOS 7

Installing required packages:
```
sudo ./install-dependencies.sh
```

To compile Seastar explicitly using gcc 5, use:
```
./configure.py --compiler=/opt/scylladb/bin/g++ --static-stdc++
ninja-build
```
