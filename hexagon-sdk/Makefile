# Hexagon V79 (SM8750) 1024-bit HVX SDK Makefile
# Builds both the Hexagon V79 CDSP shared library (libhvx_kernels_skel.so)
# and the Android bionic host runner (hexagon-hvx-bench) using upstream LLVM Clang + LLD.

CLANG ?= clang
LLD   ?= ld.lld

HEXAGON_FLAGS := --target=hexagon-unknown-elf -mv79 -mhvx -mhvx-length=128B -O3 -fPIC -fno-builtin -nostdlib
ANDROID_FLAGS := --target=aarch64-linux-android31 -O3 -fPIE

.PHONY: all skel probe clean

all: libhvx_kernels_skel.so hexagon-probe

libhvx_kernels_skel.so: hvx_kernels_skel.c hexagon-skel.lds
	$(CLANG) $(HEXAGON_FLAGS) -c hvx_kernels_skel.c -o hvx_kernels_skel.o
	$(LLD) -shared -T hexagon-skel.lds -o $@ hvx_kernels_skel.o
	rm -f hvx_kernels_skel.o

hexagon-probe: hexagon-probe.c
	cc -O2 -Wall hexagon-probe.c -o $@

clean:
	rm -f libhvx_kernels_skel.so hvx_kernels_skel.o hexagon-probe
