## Building with a DPDK network backend

 1. Set up the host to compile DPDK:
    - Ubuntu
         `sudo apt-get install -y build-essential linux-image-extra-$(uname -r)`
 2. Configure the project with DPDK enabled: `./configure.py --mode=release --enable-dpdk`
 3. Run `ninja -C build/release`.

To run a Seastar application with the DPDK backend for the native stack, pass the `--dpdk-pmd` option.

You can also configure DPDK as an [external package](../README-DPDK.md).
