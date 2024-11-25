Seastar Native TCP/IP Stack
---------------------------

Seastar comes with a native, sharded TCP/IP stack.  Usually it is used with the [DPDK](building-dpdk.md) environment, but there are also vhost drivers for testing in a development environment.

To enable the native network stack, pass the `--network-stack native` parameter to a seastar application.

To test the native stack without dpdk, install and start the `libvirt` daemon.  This will create a bridge device named `virbr0`, which seastar will connect to.

Seastar's vhost driver will need a tap device to connect to.  The scripts `scripts/tap.sh` will set up a tap device and bind it to `virbr0`:

	$ sh ./scripts/tap.sh
	Set 'tap0' nonpersistent
	bridge name	bridge id		STP enabled	interfaces
	virbr0		8000.5254008be729	no		tap0
								virbr0-nic
	virbr0: flags=4163<UP,BROADCAST,RUNNING,MULTICAST>  mtu 1500
	        inet 192.168.122.1  netmask 255.255.255.0  broadcast 192.168.122.255
	        ether 52:54:00:8b:e7:29  txqueuelen 1000  (Ethernet)
	        RX packets 384938  bytes 21866184 (20.8 MiB)
	        RX errors 0  dropped 0  overruns 0  frame 0
	        TX packets 547098  bytes 2508723098 (2.3 GiB)
	        TX errors 0  dropped 0 overruns 0  carrier 0  collisions 0

You can now run a seastar application; for example, the http server:

	$ ./build/release/apps/httpd/httpd --network-stack native
	DHCP sending discover
	DHCP Got offer for 192.168.122.18
	DHCP sending request for 192.168.122.18
	DHCP Got ack on request
	DHCP  ip: 192.168.122.18
	DHCP  nm: 255.255.255.0
	DHCP  gw: 192.168.122.1
	Seastar HTTP server listening on port 10000 ...

You can now ping the IP address shown (`192.168.122.18`) or connect to it:

	$ ping 192.168.122.18
	PING 192.168.122.18 (192.168.122.18) 56(84) bytes of data.
	64 bytes from 192.168.122.18: icmp_seq=1 ttl=64 time=0.160 ms
	64 bytes from 192.168.122.18: icmp_seq=2 ttl=64 time=0.110 ms
	64 bytes from 192.168.122.18: icmp_seq=3 ttl=64 time=0.116 ms
	64 bytes from 192.168.122.18: icmp_seq=4 ttl=64 time=0.112 ms
	64 bytes from 192.168.122.18: icmp_seq=5 ttl=64 time=0.093 ms
	64 bytes from 192.168.122.18: icmp_seq=6 ttl=64 time=0.108 ms
	^C
	--- 192.168.122.18 ping statistics ---
	6 packets transmitted, 6 received, 0% packet loss, time 4999ms
	rtt min/avg/max/mdev = 0.093/0.116/0.160/0.023 ms
	
	$ curl http://192.168.122.18:10000/
	"hello"

