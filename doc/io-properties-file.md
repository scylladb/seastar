## Specifying the I/O properties of a system

The I/O properties of a system can be specified as a YAML string, by
using the option --io-properties, or as a YAML file with the option
--io-properties-file.

The expected format starts with a map of sections at the top level.
Currently only `disks` is supported.

# The disks section

Inside the `disks` section, the user can specify a list of mount points.
Seastar will for now only accept a single mount point, but that should
change as we move forward to support multiple I/O Schedulers.

Aside from the mount point, 4 properties have to be specified (none are
optional):

`read_reqs_per_sec`: read IOPS speed of the device
`write_reqs_per_sec`: write IOPS speed of the device
`reads_bytes_per_sec`: read bandwidth speed of the device
`write_bytes_per_sec`: write bandwidth speed of the device

Those quantities can be specified in raw form, or proceeded with a
suffix (k, M, G, or T).

Example:

```
disks:
  - mountpoint: /var/lib/some_seastar_app
    read_reqs_per_sec: 95000
    write_reqs_per_sec: 85000
    read_bytes_per_sec: 545M
    write_bytes_per_sec: 510M
```
