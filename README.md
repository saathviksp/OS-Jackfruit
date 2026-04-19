# Supervised Multi-Container Runtime with Kernel Memory Monitor
 
## Team Members

* PES1UG24CS394 – Saathvik Shenoy Padubidri
* PES1UG24CS398 – Sai Harshith R

---

## Overview

This project implements a lightweight container runtime in user space along with a Linux kernel module for monitoring and enforcing memory limits on containers.

The runtime supports container creation and isolation using the `clone()` system call and Linux namespaces. It also provides container lifecycle management, logging, and inter-process communication between client and supervisor.

The system consists of:

* User-space runtime (`engine.c`)
* Kernel module (`monitor.c`)

---
## Engineering Analysis

1. Isolation Mechanisms

The runtime uses Linux namespaces and chroot() for isolation.
PID namespaces (CLONE_NEWPID) give each container its own process tree, so processes inside cannot see host processes. UTS namespaces (CLONE_NEWUTS) isolate system identity, allowing each container to have its own hostname. Mount namespaces (CLONE_NEWNS) isolate filesystem changes.

chroot() restricts the container's filesystem view to its rootfs, and /proc is mounted inside for process visibility within the namespace.

However, all containers still share the same host kernel, including the scheduler, memory management, and system calls.

2. Supervisor and Process Lifecycle

A long-running supervisor manages all containers. It creates them using clone(), tracks metadata (ID, PID, state), and handles lifecycle events.

Containers are child processes of the supervisor. When they exit, the supervisor reaps them using waitpid() to avoid zombies. It also handles signals—sending SIGTERM for graceful shutdown and SIGKILL if needed.

This centralized control simplifies management and cleanup.

3. IPC, Threads, and Synchronization

The runtime uses a UNIX domain socket for communication between client and supervisor (control plane).

For logging, it uses a bounded buffer with a producer-consumer model. Multiple producers (container events) and a consumer thread (logger) share this buffer.

Race conditions are avoided using a mutex for mutual exclusion and condition variables to handle full/empty buffer states. This ensures safe and efficient synchronization without busy waiting.

4. Memory Management and Enforcement

RSS (Resident Set Size) measures the physical memory a process is using in RAM, but not swapped-out memory.

Soft limits act as thresholds for warning or pressure, while hard limits are strict caps that trigger enforcement.

Enforcement must be in kernel space because only the kernel has full control over memory usage and can reliably enforce limits. User-space alone cannot guarantee this.

5. Scheduling Behavior

Linux's Completely Fair Scheduler distributes CPU time fairly among processes. CPU-bound workloads share CPU evenly, while processes with lower nice values get higher priority.

I/O-bound processes are scheduled quickly to maintain responsiveness.

---
## Screenshots

![1](https://github.com/user-attachments/assets/bfc43dc7-5c22-4e37-8f68-d8871ef1213e)
![2](https://github.com/user-attachments/assets/31c61280-c9b6-4f29-9331-48a89642bc85)
![3](https://github.com/user-attachments/assets/89fc1d37-bbe2-4107-80ea-9e316638c416)
![4](https://github.com/user-attachments/assets/45cba321-d936-4836-864c-5715a7c1ea2a)
![5](https://github.com/user-attachments/assets/9eabb51c-a09a-48e8-b908-9a4bc818a280)
![6](https://github.com/user-attachments/assets/7be2a585-c9c5-4523-8992-04c76e884e3f)
![7](https://github.com/user-attachments/assets/563774d2-6226-450b-bccc-7804a5c60064)
![8](https://github.com/user-attachments/assets/f6a797b2-4404-4691-bfbc-25a791a0e847)
![9](https://github.com/user-attachments/assets/64062a3b-8c61-4586-9ca9-27b9a51dff90)

---

## Architecture

### User Space (engine.c)

The user-space runtime is responsible for managing containers and handling client requests.

Key components:

* Supervisor process using UNIX domain sockets
* Client-server request/response communication
* Container lifecycle management:

  * start
  * stop
  * ps (list containers)
  * logs
* Logging system:

  * Bounded buffer (producer-consumer model)
  * Dedicated logging thread
  * File-based logs (`logs/<container>.log`)
* Graceful shutdown using signals (SIGTERM followed by SIGKILL if required)

---

### Containerization

Each container is created using:

* `clone()` system call
* Linux namespaces:

  * PID namespace (isolated process IDs)
  * UTS namespace (separate hostname)
  * Mount namespace (independent filesystem view)

Additional setup inside the container:

* `chroot()` for filesystem isolation
* Minimal root filesystem (`rootfs`)
* Mounting `/proc` to access system information within the container

---

### Kernel Space (monitor.c)

The kernel module is responsible for monitoring container memory usage.

Key components:

* Character device: `/dev/container_monitor`
* Linked list to track monitored containers
* Periodic monitoring using a kernel timer
* Memory limits:

  * Soft limit: generates warnings (visible via `dmesg`)
  * Hard limit: terminates the container process
* Synchronization using mutexes
* Communication with user space via `ioctl`

---

## Features

* Multi-container support
* Container isolation using namespaces (PID, UTS, mount)
* Process creation using `clone()`
* Filesystem isolation using `chroot()`
* `/proc` mounting inside containers
* Memory monitoring and enforcement (kernel module)
* Logging system with thread and bounded buffer
* File-based persistent logs
* Inter-process communication using UNIX domain sockets
* Structured request-response protocol between client and supervisor
* Graceful container shutdown

---

## How to Run

### Build

```bash
make
```

### Load Kernel Module

```bash
sudo insmod monitor.ko
```

### Start Supervisor

```bash
sudo ./engine supervisor rootfs
```

### Start Container

```bash
sudo ./engine start c1 rootfs /bin/bash
```

### Inside Container (Example)

```bash
echo $$
echo $(</proc/sys/kernel/hostname)
```

### List Containers

```bash
sudo ./engine ps
```

### View Logs

```bash
sudo ./engine logs c1
```

### Stop Container

```bash
sudo ./engine stop c1
```

---

## Expected Output

* Container starts successfully with a unique PID (host side)
* Inside the container:

  * PID appears as 1 (PID namespace isolation)
  * Hostname matches container ID (UTS namespace isolation)
* Logs are printed via the logging system
* Log files are created under `logs/`
* Kernel messages related to monitoring are visible using `dmesg`

---

## Project Structure

* `engine.c` – User-space container runtime
* `monitor.c` – Kernel module for memory monitoring
* `monitor_ioctl.h` – IOCTL interface definitions
* `Makefile` – Build configuration
* `logs/` – Directory for container logs
* `rootfs/` – Minimal filesystem used for containers

---
## Design Decisions and Tradeoffs

Namespace Isolation: We used Linux namespaces (PID, mount, UTS, etc.) so each container gets its own isolated view of processes and filesystem. This ensures strong separation between host and container environments. The main tradeoff is that debugging becomes more difficult because processes and resources are hidden within separate namespace contexts. We chose this approach because it is lightweight, secure, and the standard mechanism used by Linux containers, avoiding the need for heavier virtualization.

Supervisor Architecture: We implemented a long-running supervisor process that is responsible for creating, tracking, and cleaning up child container processes. This centralizes lifecycle management and ensures controlled execution. The tradeoff is that the supervisor becomes a single point of failure and must be kept reliable. We chose this design because it simplifies orchestration and gives a single control layer for all container operations.

IPC/Logging: We used a simple inter-process communication and centralized logging mechanism between the supervisor and containers. This makes it easy to trace execution flow and debug issues since all logs are collected in one place. The tradeoff is reduced performance and flexibility compared to lower-level IPC mechanisms like shared memory or advanced message queues. We chose this design because clarity and debuggability were more important than raw performance for this project.

Kernel Monitor: We built a lightweight monitoring system that observes process and resource usage externally rather than modifying the kernel. This provides visibility into system behavior without deep kernel integration. The tradeoff is that the monitoring is approximate and may not capture fine-grained or real-time kernel-level metrics. We chose this approach because it avoids kernel complexity while still providing useful runtime insights.

Scheduling Experiments: We experimented with Linux scheduling policies by adjusting process priorities and observing behavior under different workloads. This allowed us to study scheduling without modifying the kernel itself. The tradeoff is that results can vary depending on host system load and are not fully deterministic. We chose this approach because it is safe, portable, and sufficient for demonstrating scheduling concepts without kernel-level modifications.

---

## Conclusion

This project demonstrates the design and implementation of a simplified container runtime using Linux system programming concepts.

It integrates:

* Process creation with `clone()`
* Namespace-based isolation
* Filesystem isolation using `chroot()`
* Kernel-user communication
* Memory monitoring via a kernel module
* IPC and concurrent logging mechanisms

The system provides a basic Docker-like environment implemented from scratch, highlighting core operating system concepts in practice.
