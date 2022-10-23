Seastar and DPDK
================

Seastar uses the Data Plane Development Kit to drive NIC hardware directly.  This
provides an enormous performance boost.

To enable DPDK, specify `--enable-dpdk` to `./configure.py`, and `--dpdk-pmd` as a
run-time parameter.  This will use the DPDK package provided as a git submodule with the
seastar sources.

Please note, if `--enable-dpdk` is used to build DPDK on an aarch64 machine, you need to
specify [target architecture](https://gcc.gnu.org/onlinedocs/gcc/AArch64-Options.html) with optional
[feature modifiers](https://gcc.gnu.org/onlinedocs/gcc/AArch64-Options.html#aarch64-feature-modifiers)
with the `--cflags` option as well, like:
```console
$ ./configure.py --mode debug --enable-dpdk --cflags='-march=armv8-a+crc+crypto'
```

To use your own self-compiled DPDK package, follow this procedure:

1. Setup host to compile DPDK:
   - Ubuntu 
     `sudo apt-get install -y build-essential linux-image-extra-$(uname -r)` 
2. Prepare a DPDK SDK:
   - Download the latest DPDK release: `wget http://dpdk.org/browse/dpdk/snapshot/dpdk-2.0.0.tar.gz`
   - Untar it.
   - Edit config/common_linuxapp: set CONFIG_RTE_MBUF_REFCNT_ATOMIC and CONFIG_RTE_LIBRTE_KNI to 'n'.
   - Start the tools/setup.sh script as root.
   - Compile a linuxapp target (option 9).
   - Install IGB_UIO module (option 12).
   - Bind some physical port to IGB_UIO (option 18).
   - Configure hugepage mappings (option 15/16).
3. Modify the CMake cache (`CMakeCache.txt`) to inform CMake of the location of the installed DPDK SDK.
