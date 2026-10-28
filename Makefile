# SPDX-License-Identifier: GPL-3.0-or-later
#
# make linux           -> build/usbeam                (Linux x86_64, static)
# make windows-arm64   -> build/usbeam-host-arm64.exe (needs llvm-mingw in PATH)
# make windows-x64     -> build/usbeam-host-x64.exe   (needs llvm-mingw in PATH)

CFLAGS ?= -O2 -Wall
WINLIBS = -lsetupapi -lwinusb -lws2_32 -lcfgmgr32

all: linux windows-arm64 windows-x64

build:
	mkdir -p build

linux: build
	gcc $(CFLAGS) -static -o build/usbeam src/linux/usbeam.c

windows-%: build
	$(eval ARCH := $(if $(filter arm64,$*),aarch64,x86_64))
	cd src/windows && $(ARCH)-w64-mingw32-windres app.rc -O coff -o ../../build/app-$*.res
	$(ARCH)-w64-mingw32-gcc $(CFLAGS) -Wno-missing-field-initializers src/windows/usbeam_host.c build/app-$*.res \
		-o build/usbeam-host-$*.exe $(WINLIBS)

clean:
	rm -rf build

.PHONY: all linux clean
