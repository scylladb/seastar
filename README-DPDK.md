Seastar and DPDK
================

Seastar uses the Data Plane Development Kit to drive NIC hardware directly.  This
provides an enormous performance boost.

To enable DPDK, pass `--enable-dpdk` to `./configure.py` and `--dpdk-pmd` to the
Seastar application at run time. This uses the DPDK package provided as a Git submodule
with the Seastar sources.

If `--enable-dpdk` is used to build DPDK on an AArch64 machine, you also need to
specify a [target architecture](https://gcc.gnu.org/onlinedocs/gcc/AArch64-Options.html) with optional
[feature modifiers](https://gcc.gnu.org/onlinedocs/gcc/AArch64-Options.html#aarch64-feature-modifiers)
with the `--cflags` option as well, like:
```console
$ ./configure.py --mode debug --enable-dpdk --cflags='-march=armv8-a+crc+crypto'
```

To use your own self-compiled DPDK package, follow this procedure:

1. Set up the host to compile DPDK:
   - Ubuntu
     `sudo apt-get install -y build-essential linux-image-extra-$(uname -r)`
2. Prepare a DPDK SDK:
   - Download a DPDK release (for example, `wget https://fast.dpdk.org/rel/dpdk-23.07.tar.xz`).
   - Untar it.
   - Follow the [Quick Start Guide](https://core.dpdk.org/doc/quick-start/)
   - Pass `-Dmbuf_refcnt_atomic=false` to Meson.
3. Modify the CMake cache (`CMakeCache.txt`) to inform CMake of the location of the installed DPDK SDK.
