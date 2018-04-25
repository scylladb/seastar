# Specifying the I/O properties of a system

The I/O properties of a system can be specified as a YAML string, by
using the option --io-properties, or as a YAML file with the option
--io-properties-file.

The expected format starts with a map of sections at the top level.
Currently only `disks` is supported.

## The disks section

Inside the `disks` section, the user can specify a list of mount points.
Seastar will for now only accept a single mount point, but that should
change as we move forward to support multiple I/O Schedulers.

Aside from the mount point, 4 properties have to be specified (none are
optional):

* `read_iops`: read IOPS speed of the device
* `reads_bandwidth`: read bandwidth speed of the device
* `write_iops`: write IOPS speed of the device
* `write_bandwidth`: write bandwidth speed of the device

Those quantities can be specified in raw form, or proceeded with a
suffix (k, M, G, or T).

Example:

```
disks:
  - mountpoint: /var/lib/some_seastar_app
    read_iops: 95000
    read_bandwidth: 545M
    write_iops: 85000
    write_bandwidth: 510M
```
