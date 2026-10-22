# DuckDB as a miniOSv unikernel.
#
# This repository is the application; the kernel is the submodule in kernel/.
# A build starts here:
#
#     make                  the image, for this host's architecture
#     make arch=aarch64     cross-build
#     make run              boot it under qemu
#     make clean
#
# Variables set on the command line reach the kernel's Makefile as they are,
# so `make conf_pagecache_stats=1` and the like work from here too. Running
# the image usually wants arguments; they go through RUN_FLAGS:
#
#     make run RUN_FLAGS='-m 32G --emulated-nvme /scratch/tpch.img \
#                         --args "benchmark --sf 10 ^Q06$$"'
#
# The kernel's own Makefile includes this file in turn, with `app` set, to
# learn which objects the application contributes. That is the first branch
# below; the driver after it never runs during a kernel build.

ifdef app

# Included by the kernel. miniosv/miniosv.mk puts the objects in
# $(app-objects) and finds this tree relative to its own path.
include $(dir $(lastword $(MAKEFILE_LIST)))miniosv/miniosv.mk

else

kernel    := kernel
RUN_FLAGS ?=

# The kernel compiles the application from here, so it needs this tree's
# absolute path: object paths derive from it, and so does the linker pattern
# that defers the application's constructors.
app := $(CURDIR)

.PHONY: all run clean
all:
	+$(MAKE) -C $(kernel) app=$(app)

run: all
	$(kernel)/scripts/run.py $(RUN_FLAGS)

clean:
	+$(MAKE) -C $(kernel) app=$(app) clean

endif
