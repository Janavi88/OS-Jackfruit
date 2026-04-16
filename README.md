# Multi-Container Runtime

A lightweight Linux container runtime in C with a long-running supervisor and a kernel-space memory monitor.

## Team Information

* Janavi Agrawal (SRN: PES1UG24AM120)
* Teammate Name (SRN: PES1UG24AM112)


## Project Overview

This project implements a lightweight Linux container runtime in C with:

* A long-running supervisor process
* Support for multiple containers
* CLI-based interaction using IPC


## Completed Tasks

* Task 1: Multi-container runtime using `clone()`
* Task 2: Supervisor CLI and IPC mechanism

---

## In Progress

* Task 3: Bounded-buffer logging system

---

## Basic Build & Run

```bash
make
sudo insmod monitor.ko

# Start supervisor
sudo ./engine supervisor ./rootfs-base

# Start container
sudo ./engine start alpha ./rootfs-alpha /bin/sh

# List containers
sudo ./engine ps
```

---

## Notes

* Each container runs in isolated namespaces
* Supervisor manages lifecycle and metadata
* Full documentation and analysis will be added later


