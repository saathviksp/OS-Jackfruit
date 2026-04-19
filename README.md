# Supervised Multi-Container Runtime with Kernel Memory Monitor

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

### 1. Boilerplate directory — all files present after build

![ss1](https://github.com/user-attachments/assets/a082306f-0fb4-4a79-b00c-8aaf621d9e1a)

### 2. Kernel module loaded — dmesg confirms device created, supervisor starts

![ss3-1](https://github.com/user-attachments/assets/6b4072c3-869f-401b-8395-4f2454685400)

### 3. Full CLI demo — start, ps, logs (hello), stop

![ss3-2](https://github.com/user-attachments/assets/4c4cda50-5467-4c9d-8de1-5a90b49dd66e)

### 4. Two containers running simultaneously — alpha and beta both show running

![ss4](https://github.com/user-attachments/assets/8b378962-03eb-49ae-b007-804d41a42837)

### 5. Supervisor terminal — containers registered and started

![s44-2](https://github.com/user-attachments/assets/0a9e0b24-871f-482e-9d3b-30731d836623)

### 6. Log capture — container log test and line 2 from bounded buffer pipeline

![ss7-2](https://github.com/user-attachments/assets/ea3bfffa-753e-469e-8f30-47d0c1aa5cec)

### 7. Soft limit warning — dmesg shows SOFT LIMIT for memtest

![ss7](https://github.com/user-attachments/assets/f2ecb526-ee9e-4653-8f27-8b9d173ec96c)

### 8. Hard limit kill — dmesg shows HARD LIMIT for memtest and killme, both terminated

![soft-hard](https://github.com/user-attachments/assets/cc96f2e7-4cbc-4713-bc45-085d048c255f)

### 9. Scheduling experiment — fast (nice 0) and slow (nice 15) running concurrently

![soft-nice](https://github.com/user-attachments/assets/e6215c32-7220-4ff5-a427-0e3f23660188)

### 10. Clean teardown — no zombie processes after supervisor shutdown

![ss8](https://github.com/user-attachments/assets/f087aa1f-3f72-43b0-9501-a8f223708a9a)

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
