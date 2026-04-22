

## Team Members

* PES1UG24CS394 – Saathvik Shenoy Padubidri
* PES1UG24CS398 – Sai Harshith R

---

## Overview

This project builds a lightweight container runtime from scratch in C, paired with a Linux kernel module that monitors and enforces per-container memory limits. Containers are isolated using Linux namespaces and chroot, managed by a long-running supervisor process, and communicate with a CLI client over a UNIX domain socket. A bounded producer-consumer buffer handles container log capture, and a kernel timer enforces soft and hard memory limits from kernel space.

The system has two main components:

* User-space runtime (`engine.c`)
* Kernel memory monitor (`monitor.c`)

---

## Engineering Analysis

### 1. Isolation Mechanisms

Each container is created with `clone()` using three namespace flags. `CLONE_NEWPID` gives the container its own PID namespace so processes inside see themselves starting from PID 1 and cannot enumerate or signal host processes. `CLONE_NEWUTS` isolates the system hostname, which we set to the container ID using `sethostname()`. `CLONE_NEWNS` gives the container a private mount namespace so filesystem mounts inside do not affect the host.

`chroot()` then restricts the container's filesystem view to the Alpine rootfs directory. We mount `/proc` inside the container so it can observe its own process tree. The host mount table is not affected.

All containers still share the same host kernel, network stack, and IPC namespace. A production runtime like Docker adds network and IPC namespaces and uses `pivot_root` for stronger isolation — these are known limitations of our implementation.

### 2. Supervisor and Process Lifecycle

The supervisor is a long-running process that manages all containers. It uses `clone()` rather than `fork()` so namespace flags are applied atomically at creation time. The child stack is heap-allocated and passed as a pointer to the top since x86-64 stacks grow downward.

After each container starts, the parent closes its end of the write pipe so the pipe reaches EOF when the child exits, cleanly terminating the log reader thread. Container reaping uses `SIGCHLD` with `SA_RESTART | SA_NOCLDSTOP`. The signal handler sets a flag and the event loop calls `waitpid(-1, WNOHANG)` in a loop to drain all exited children without blocking. This prevents zombie processes from accumulating in the process table.

On `SIGINT` or `SIGTERM`, the supervisor sends `SIGTERM` to all running containers, waits two seconds, reaps remaining children, shuts down the logger thread, frees all memory, closes file descriptors, and removes the socket file.

### 3. IPC, Threads, and Synchronization

The CLI and supervisor communicate over a UNIX domain socket at `/tmp/mini_runtime.sock`. UNIX sockets were chosen over FIFOs because they are bidirectional and connection-oriented, making it easy to match each CLI invocation to its response even if multiple clients connect at once. The supervisor uses `select()` with a one-second timeout to multiplex between the socket and signal flags without blocking.

Container stdout and stderr are captured through a pipe into a per-container log reader thread (producer), which pushes fixed-size chunks into a shared `bounded_buffer_t`. A single consumer thread pops chunks and writes them to per-container log files under `logs/`. The buffer is protected by a `pthread_mutex_t` with two condition variables — `not_empty` and `not_full` — so neither side busy-waits. A `shutting_down` flag lets the consumer drain remaining items before exiting rather than dropping them.

The container metadata linked list is protected by a separate `metadata_lock` mutex, guarding against races between the SIGCHLD reaper and CLI handler paths.

### 4. Memory Management and Enforcement

The kernel module reads each container's Resident Set Size using `get_mm_rss()` on the task's `mm_struct`. RSS counts physical pages currently mapped into the process — it excludes swapped pages and is the best measure of immediate memory pressure on the host.

Enforcement happens in kernel space because a user-space monitor cannot reliably kill a process that allocates memory faster than the monitor polls. The kernel timer fires every second regardless of whether the supervised process is scheduled. `send_sig(SIGKILL, task, 1)` bypasses the signal mask, making hard-limit enforcement impossible for the container to block.

The soft limit emits a `KERN_WARNING` via `printk` once per entry using a `soft_warned` flag to avoid log spam. The hard limit kills the process and removes its entry from the monitored list using `list_for_each_entry_safe` to avoid use-after-free during deletion.

A mutex is used instead of a spinlock because `get_task_mm()` and `mmput()` can sleep, which is forbidden inside a spinlock critical section. Both the timer callback and the ioctl handler run in sleepable contexts so a mutex is correct on both paths.

### 5. Scheduling Behavior

Linux uses the Completely Fair Scheduler. Each runnable task accumulates virtual runtime and CFS always picks the task with the lowest vruntime. Nice values adjust the rate at which vruntime advances — a task at nice 0 accumulates vruntime more slowly than one at nice 15, so it is selected more often and receives more real CPU time.

In our experiment, `fast` (nice 0) and `slow` (nice 15) ran simultaneously with `/cpu_hog`. Over the observation window, `fast` received significantly more CPU time than `slow`, consistent with the weight ratio defined by the CFS nice-to-weight table. A separate test with a CPU-bound and memory-bound container showed that the memory-bound container naturally yielded CPU during page faults, confirming that CFS does not penalize tasks that block voluntarily.

---

## Screenshots

### 1.	Two containers (alpha and beta) running concurrently under a single supervisor process.

![ss1](https://github.com/user-attachments/assets/a49ca265-e42d-4889-ae37-769d44058e6b)

### 2.Supervisor tracking container metadata including ID, PID, state, and exit status.

![ss3-1](https://github.com/user-attachments/assets/536c6890-e176-43a3-9baf-93b8e3e6ce27)

### 3.	Container output captured through logging pipeline using producer-consumer bounded buffer.
![ss3-2](https://github.com/user-attachments/assets/359037e1-9cd7-46f0-afe8-cc2bb74b7f45)

### 4.4.	CLI sending command to supervisor via UNIX socket IPC and receiving response.

![ss4](https://github.com/user-attachments/assets/ff85e168-d956-4f10-9f53-2cba483d650c)
![ss4](https://github.com/user-attachments/assets/02e71a4d-5683-48fa-a31c-db63303e90bd)

5,6. Soft and hard memory limit enforcement: The kernel module logs a warning when the container exceeds the soft memory limit and terminates the container using SIGKILL when the hard limit is exceeded confirming forced kill in the metadata.
![](https://github.com/user-attachments/assets/572e2a25-949a-41c6-9842-289d00bafc77 )
![](https://github.com/user-attachments/assets/f2bab92d-d200-4a5e-adbd-cf8f71a3f7cc)
![](https://github.com/user-attachments/assets/5d8a8d7b-4087-4c15-a6dc-454c490d8f6f)
![](https://github.com/user-attachments/assets/e1558384-4f41-44e9-acd4-60608f942e2c)

### 7.	Scheduling experiment comparing CPU-bound (cpu_test) and I/O-bound (io_test) workloads. The CPU-bound process continuously consumes CPU and produces frequent output, while the I/O-bound process performs periodic operations with delays due to sleep, resulting in spaced output. This demonstrates how the Linux scheduler maintains responsiveness for I/O-bound processes.
![ss7](https://github.com/user-attachments/assets/1d55c804-3201-4063-b236-6ee2b2cf4760)

### 8.	Clean teardown demonstrating that all containers are properly terminated, the supervisor exits cleanly, and no zombie or residual processes remain in the system.
![soft-hard](https://github.com/user-attachments/assets/6f349f10-e628-4885-b04b-63f188aaf533)
![soft-hard](https://github.com/user-attachments/assets/92f8828a-6461-4290-8c35-860087a4e9e9)


---

## Architecture

### User Space (engine.c)

The user-space runtime manages containers and handles all client requests.

Key components:

* Long-running supervisor using a UNIX domain socket for CLI communication
* Fixed-size request and response structs sent over the socket
* Container lifecycle: start, stop, ps, logs
* Bounded producer-consumer buffer with a dedicated logger thread
* Per-container log files written to `logs/<id>.log`
* Graceful shutdown via SIGTERM then SIGKILL with a timeout

### Containerization

Each container is created with:

* `clone()` with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD`
* `chroot()` into the Alpine rootfs
* `sethostname()` set to the container ID
* `/proc` mounted inside for process visibility
* stdout and stderr redirected through a pipe to the supervisor logging pipeline

### Kernel Space (monitor.c)

The kernel module monitors container memory usage from kernel space.

Key components:

* Character device at `/dev/container_monitor`
* Kernel linked list of monitored entries protected by a mutex
* Periodic `struct timer_list` callback firing every second
* Soft limit: one-time `KERN_WARNING` via `printk`
* Hard limit: `send_sig(SIGKILL)` then entry removed
* Register and unregister via `ioctl` from the supervisor

---

## Features

* Multi-container support under a single supervisor
* PID, UTS, and mount namespace isolation per container
* Filesystem isolation via `chroot()` into Alpine rootfs
* `/proc` mounted inside each container
* Kernel-enforced soft and hard memory limits
* Producer-consumer logging with bounded buffer and condition variables
* Per-container log files persisted to disk
* UNIX domain socket CLI with structured request/response protocol
* Graceful shutdown with full resource cleanup

---

## How to Run

### Build

```bash
cd boilerplate
make clean
make
```

### Set up rootfs

```bash
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs
sudo cp cpu_hog io_pulse memory_hog rootfs/
```

### Load kernel module

```bash
sudo insmod monitor.ko
ls /dev/container_monitor
sudo dmesg | tail -5
```

### Start supervisor (terminal 1)

```bash
sudo ./engine supervisor ./rootfs
```

### Use CLI (terminal 2)

```bash
sudo ./engine start alpha ./rootfs "echo hello && sleep 30"
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha
```

### Memory limit test

```bash
sudo ./engine start memtest ./rootfs "/memory_hog" --soft-mib 5 --hard-mib 10
sleep 10
sudo dmesg | tail -10
```

### Scheduling experiment

```bash
sudo ./engine start fast ./rootfs "/cpu_hog" --nice 0
sudo ./engine start slow ./rootfs "/cpu_hog" --nice 15
sleep 5
sudo ./engine ps
```

### Unload module

```bash
sudo rmmod monitor
sudo dmesg | tail -5
```

---

## Project Structure

* `engine.c` — user-space supervisor and CLI
* `monitor.c` — kernel module for memory monitoring
* `monitor_ioctl.h` — shared ioctl interface definitions
* `Makefile` — builds all user targets and the kernel module
* `logs/` — per-container log files created at runtime
* `rootfs/` — Alpine minimal filesystem used as container root

---

## Design Decisions and Tradeoffs

**`clone()` over `fork()` + `unshare()`:** `clone()` applies namespace flags atomically at creation so the child is isolated from its very first instruction. Using `fork()` then `unshare()` leaves a window where the child runs in the parent's namespaces, which breaks PID namespace semantics.

**UNIX socket over FIFO:** A FIFO requires two files for bidirectional communication and has no connection concept, making it hard to match responses to the correct CLI invocation under concurrent use. A `SOCK_STREAM` UNIX socket is connection-oriented and bidirectional over a single file descriptor.

**Single logger consumer thread:** Multiple consumer threads would need per-file locks to prevent interleaved writes to the same log file. A single consumer serializes all writes naturally. For short-lived test containers this tradeoff is correct — throughput is not the bottleneck.

**Mutex over spinlock in kernel module:** `get_task_mm()` and `mmput()` can sleep, which is forbidden inside a spinlock critical section. Both the timer callback and the ioctl handler run in sleepable process context, so a mutex is the correct choice on both paths.

**Soft limit warned once:** Emitting a warning every timer tick would flood `dmesg`. The `soft_warned` flag ensures the warning fires exactly once per container, making it actionable rather than noise.

---

## Conclusion

This project demonstrates a working container runtime built entirely from Linux system programming primitives. It integrates namespace-based process isolation, a supervised process lifecycle, thread-safe producer-consumer logging, UNIX socket IPC, and kernel-space memory enforcement — covering the core concepts of modern container technology from first principles.
