Network Configuration
---------------------

In order to support multiple network devices in Seastar, new Network Configuration format has to be introduced.
New format is Yaml based and contains a list of network devices along with its IP parameters and optional (if DPDK is used) hardware parameters.
The new configuration can be provided either by command line with --net-config option or by configuration file by --net-config-file.

### DPDK access
Network device (called port in DPDK) can be accessed by either port index ( zero based index of device shown by dpdk-setup.sh ) or its PCI address (shown by lspci, lshw tools)

Example config line with pci address given:

```
eth0: {pci_address: 0000:06:00.0, ip: 192.168.100.10, gateway: 192.168.100.1, netmask: 255.255.255.0 }
```

Example config line with port index given:

```
eth0: {port_index: 0, ip: 192.168.100.10, gateway: 192.168.100.1, netmask: 255.255.255.0 }
```

Please note that device name - eth0 above, is not used by DPDK itself, it remains only for configuration consistency.
The hardware configuration has to be specied in the same way accross all network devices, so for example if pci_address is specified for one network device, port_index cannot be specified for any other.


## Non-DPDK access
When there is no pci_address neither port_index defined then Non-DPDK access is assumed provided by libvirt deamon (see native-stack.md), eg:

```
virbr0: { ip: 192.168.100.10, gateway: 192.168.100.1, netmask: 255.255.255.0 }
```

## Other hardware related options

There are other hardware related optional options, some of the are common for both DPDK and non-DPDK mode
- lro ( large receive offload ), boolean, deafult true
- tso ( tcp segmentation offload ), boolean, default true
- ufo ( udp fragmentation offload ), boolean, default true
- hw-fc ( hardware flow control ), boolean, default true,
- csum-offload ( IP checksum offload ), boolean, default true
- ring-size ( device ring buffer size ), unsigned, default 256, libvirt only
- event-index ( VIRTIO_RING_F_EVENT_IDX	support enabled ), boolean, default true, libvirt only


## DHCP

IP configuration can be set by either IP/gateway/netmask (as seen in examples above), but also by DHCP with dhcp=true setting, eg:

```
eth0: {pci_address: 0000:06:00.0, dhcp=true}
```

DHCP can be selected per network device, so it would also be perfectly valid to define dhcp for eth0, but ip/netmask/gateway for eth1.


## Multiple devices
Configuration formay for multiple devices is a comma separated lists of single devices with added YAML brackets, eg:

```
{virbr0: { ip: 192.168.100.10, gateway: 192.168.100.1, netmask: 255.255.255.0 } , virbr1: { dhcp: true } }
```



