Network Configuration
---------------------

To support multiple network devices, Seastar uses a YAML-based network configuration
format. It contains a list of network devices with their IP parameters and, when DPDK
is used, optional hardware parameters. Provide the configuration on the command line
with `--net-config` or in a file with `--net-config-file`.

### DPDK access
A network device (called a port in DPDK) can be accessed by either its port index (the
zero-based device index shown by `dpdk-setup.sh`) or its PCI address (shown by tools
such as `lspci` and `lshw`).

Example configuration with a PCI address:

```
eth0: {pci_address: 0000:06:00.0, ip: 192.168.100.10, gateway: 192.168.100.1, netmask: 255.255.255.0 }
```

Example configuration with a port index:

```
eth0: {port_index: 0, ip: 192.168.100.10, gateway: 192.168.100.1, netmask: 255.255.255.0 }
```

The device name (`eth0` above) is not used by DPDK itself; it is retained only for
configuration consistency. Hardware configuration must use the same identifier for
all network devices. For example, if `pci_address` is specified for one device,
`port_index` cannot be specified for any other device.


## Non-DPDK access
When neither `pci_address` nor `port_index` is defined, Seastar assumes non-DPDK access
provided by the `libvirt` daemon (see the [native stack documentation](native-stack.md)):

```
virbr0: { ip: 192.168.100.10, gateway: 192.168.100.1, netmask: 255.255.255.0 }
```

## Other hardware related options

Other optional hardware-related settings are listed below. Some apply to both DPDK and non-DPDK modes.

- `lro` (large receive offload), Boolean, default: `true`
- `tso` (TCP segmentation offload), Boolean, default: `true`
- `ufo` (UDP fragmentation offload), Boolean, default: `true`
- `hw-fc` (hardware flow control), Boolean, default: `true`
- `csum-offload` (IP checksum offload), Boolean, default: `true`
- `ring-size` (device ring buffer size), unsigned integer, default: `256`; libvirt only
- `event-index` (`VIRTIO_RING_F_EVENT_IDX` support), Boolean, default: `true`; libvirt only


## DHCP

IP configuration can use either `ip`, `gateway`, and `netmask` (as shown above) or DHCP with `dhcp: true`:

```
eth0: {pci_address: 0000:06:00.0, dhcp: true}
```

DHCP can be selected per network device. For example, you can use DHCP for `eth0` and specify `ip`, `netmask`, and `gateway` for `eth1`.


## Multiple devices
To configure multiple devices, provide a comma-separated YAML map:

```
{virbr0: { ip: 192.168.100.10, gateway: 192.168.100.1, netmask: 255.255.255.0 } , virbr1: { dhcp: true } }
```

