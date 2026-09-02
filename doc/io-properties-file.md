# Specifying the I/O properties of a system

The I/O properties of a system can be specified as a YAML string, by
using the `--io-properties` option, or as a YAML file with the
`--io-properties-file` option.

The expected format starts with a map of sections at the top level.
Currently only `disks` is supported.

## The disks section

Inside the `disks` section, the user can specify a list of mount points.

For each mount point, four properties have to be specified (none are
optional):

* `read_iops`: read IOPS speed of the device
* `read_bandwidth`: read bandwidth speed of the device
* `write_iops`: write IOPS speed of the device
* `write_bandwidth`: write bandwidth speed of the device


Additionally, the following optional properties can be added:

* `read_saturation_length`: read buffer length to saturate the device throughput
* `write_saturation_length`: write buffer length to saturate the device throughput
* `physical_block_size`: override for the physical block size of the device (in bytes).
  This is used as the write alignment to avoid hardware-level read-modify-write operations.
  Some devices misreport their physical block size, so this override can be used to
  specify the correct value

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

Instead of `mountpoint`, a list element can contain a `mountpoints` list with more
than one path. As a result, all the listed mount points share the corresponding
internal I/O queue.

Example:

```
disks:
  - mountpoints:
      - /var/lib/some_seastar_app/sub_one
      - /var/lib/some_seastar_app/sub_two
    read_iops: 95000
    ...
```

This configuration is useful, for example, for a set of LVM volumes from one disk,
each mounted at its own path. In that case, different mount points have different
virtual block devices but share the same physical disk, so they need to use one
shared I/O queue.
