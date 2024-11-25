## Building with a DPDK network backend

 1. Setup host to compile DPDK:
    - Ubuntu
         `sudo apt-get install -y build-essential linux-image-extra-$(uname -r)`
 2. Configure the project with DPDK enabled: `./configure.py --mode=release --enable-dpdk`
 3. Run `ninja-build build/release`.

To run with the DPDK backend for a native stack give the seastar application `--dpdk-pmd 1` parameter.

You can also configure DPDK as an [external package](/README-DPDK.md).
