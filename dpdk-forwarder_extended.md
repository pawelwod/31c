Task: Implement a Simple DPDK-Based Packet Forwarder

You are required to develop a DPDK-based application that captures packets from one network interface and forwards them to another.

Requirements:
	1.	Environment Setup:
	•	The application should be written in C.
	•	It should use DPDK for high-performance packet processing.
	•	The program should be able to run on a Linux system with DPDK installed.
	2.	Core Functionality:
	•	Initialize DPDK and allocate memory pools for packet buffers.
	•	Bind to two network interfaces (e.g., eth0 and eth1).
	•	Receive packets from one interface, process them, and forward them to another interface.
	•	Maintain basic packet statistics such as received, forwarded, and dropped packets.
    •   Bonus: collect rx and tx bytes and rx/tx bitrates.
	3.	Packet Processing Logic:
	•	Parse incoming packets to extract basic Ethernet/IP header information.
	•	Drop packets if they are non-IP packets.
	•	Optionally modify the packet headers (e.g., change the destination MAC address).
	•	Forward valid packets to the appropriate interface.
	4.	Performance Considerations:
	•	Use multiple RX/TX queues to enable parallel processing by multiple worker threads.
	•	Optimize memory access patterns for cache efficiency.
	•	Implement basic batch processing for packets.
	5.  Periodic real time traffic statistics:
	•   Implement periodic statistics aggregation across all RX/TX queues and print them to screen.
	•   Statistics collection period should be configurable.
	•   Statistics should be displayed as a total summary across all queues in real time - not per thread/queue.
	•   Bonus: Implement this without using locks. Some loss of accuracy is acceptable when implementing a lockless solution.
	6.	Logging & Debugging:
	•	Implement logging for debugging purposes.
	

Expected Deliverables:
	•	A C program implementing the packet forwarder.
	•	A README.md with:
	•	Instructions on how to compile and run the program.
	•	List of dependencies.
	•	Expected output and how to verify the program is working.
	•	Optional: A brief performance report measuring throughput with different packet sizes.

Bonus Challenges (Optional but Appreciated):
	1.	Implement a simple rate limiter to control the packet forwarding rate.
	2.	Add a basic filtering mechanism to drop packets from certain IP addresses.
	3.	Use multiple cores for improved performance (e.g., separate RX and TX threads).

Evaluation Criteria:
	•	Code correctness & stability
	•	Understanding of DPDK concepts
	•	Code efficiency & performance optimization
	•	Adherence to best practices in low-level networking
	•	Quality of documentation