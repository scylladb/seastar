# Specifying the I/O properties of a system

The I/O properties of a system can be specified as a YAML string, by
using the option --io-properties, or as a YAML file with the option
--io-properties-file.

The expected format starts with a map of sections at the top level.
Currently only `disks` is supported.

## The disks section

Inside the `disks` section, the user can specify a list of mount points.

For each mount point, 4 properties have to be specified (none are
optional):

* `read_iops`: read IOPS speed of the device
* `read_bandwidth`: read bandwidth speed of the device
* `write_iops`: write IOPS speed of the device
* `write_bandwidth`: write bandwidth speed of the device


Additionally the following optional properties can be added:

* `read_saturation_length`: read buffer length to saturate the device throughput
* `write_saturation_length`: write buffer length to saturate the device throughput

Those quantities can be specified in raw form, or followed with a
suffix (k, M, G, or T).

Example:

```
disks:
  - mountpoint: /var/lib/some_seastar_app
    read_iops: 95000
    read_bandwidth: 545M
    write_iops: 85000
    write_bandwidth: 510M
    write_saturation_length: 64k
```

Optionally, instead of the "mountpoint" there can be the "mountpoints" (plural)
entry in the list element being a list itself and listing more than one path. As
a result all the listed mountpoints will share the corresponding internal IO queue.

Example:

```
disks:
  - mountpoints:
      - /var/lib/some_seastar_app/sub_one
      - /var/lib/some_seastar_app/sub_two
    read_iops: 95000
    ...
```

An example when this configuration is applicable can be an LVM set of volumes
from one disk each being mounted at its own path. In that case, different mount
points would have different (virtual) block devices, yet, they will share the
same physical disk and thus need to run over one shared IO queue.
