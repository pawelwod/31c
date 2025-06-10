# Dependencies


Build dependencies for DPDK 25.03.0 are sufficient to build this example.


## Build and install DPDK 25.03.0


Refer to the "Getting Started Guide for Linux" section 
[3.2. Compiling and Installing DPDK System-wide](https://doc.dpdk.org/guides-25.03/linux_gsg/build_dpdk.html#compiling-and-installing-dpdk-system-wide)


# Building application


Use the following command to build this application:


	make all
	
This will build a dynamically linked version. To build a statically linked version, use:


	make static


# Testing


## DPDK user guide
The process of running this application is analogous to the DPDK L2FWD example application.
Please read the [L2FWD use guide first](https://doc.dpdk.org/guides/sample_app_ug/l2_forward_real_virtual.html).


## Environment setup


Please reserve an appropriate amount of hugepage memory, e.g., 1024:


	dpdk-hugepages.py --setup 1024


If a NUMA system is in use, reserve memory on each node where the Ethernet port is placed:


	dpdk-hugepages.py --mount --node 0 --size 512M
	dpdk-hugepages.py --mount --node 1 --size 512M
	
Use the `dpdk-devbind.py` script to bind exactly two Ethernet ports.


## Running


To run the application, just execute:


	./build/l2fwd
	
To change the destination MAC address of the transmitted packets on TX port 0 and 
display stats with a 2-second interval:


	./build/l2fwd -- -T 2 --port0-dst-mac 00:01:10:10:10:12
	
## Debugging


One can run the application without any real hardware port, using, for example, a TAP interface:


	./build/l2fwd --no-pci --vdev=net_tap0,iface=dpdk0 --vdev=net_tap1,iface=dpdk1  -- -T 2 --port0-dst-mac 00:01:10:10:10:12