CC = x86_64-w64-mingw32-gcc
EXPERIMENTAL_GSP ?= 0
UNSAFE_GSP_PROBE ?= 0
CFLAGS = -DNVX_ENABLE_EXPERIMENTAL_GSP=$(EXPERIMENTAL_GSP) -DNVX_ENABLE_UNSAFE_GSP_PROBE=$(UNSAFE_GSP_PROBE) -Wall -Wextra -O2 -nostdlib -m64 -fno-builtin -fno-stack-protector -I.
LDFLAGS = -shared -Wl,--entry=DriverEntry -Wl,--subsystem,native:5.2 -nostartfiles -Wl,--image-base,0x10000 -Wl,--file-alignment,0x200 -Wl,--section-alignment,0x1000 -L. -lvideoprt

all: nvxmp.sys

.PHONY: FORCE
FORCE:

nvxmp.sys: FORCE

gsp_blob.o: ../dist_full/gsp_ga104.bin
	cd .. && x86_64-w64-mingw32-objcopy -I binary -O pe-x86-64 -B i386:x86-64 dist_full/gsp_ga104.bin src_driver/gsp_blob.o

nvxmp.sys: nvxmp.c nvx_gsp.c nvx_vbe.c nvx_vbe.h nvx_fw.c nvx_fw.h nvx_vbios.c nvx_vbios.h nvx_gsp.h miniport.h bootloader_blob.h booter_load_blob.h gsp_blob.o nvxmp.def libvideoprt.a
	$(CC) $(CFLAGS) -o $@ nvxmp.c nvx_gsp.c nvx_vbe.c nvx_fw.c nvx_vbios.c nvx_rpc.c gsp_blob.o nvxmp.def $(LDFLAGS)

libvideoprt.a: videoprt.def
	x86_64-w64-mingw32-dlltool -d videoprt.def -l libvideoprt.a

clean:
	rm -f nvxmp.sys libvideoprt.a gsp_blob.o

.PHONY: check
check:
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I. -o /tmp/nvx-rpc-test tests/rpc_test.c nvx_rpc.c
	ASAN_OPTIONS=detect_leaks=0 /tmp/nvx-rpc-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I. -o /tmp/nvx-firmware-test tests/firmware_test.c nvx_fw.c
	ASAN_OPTIONS=detect_leaks=0 /tmp/nvx-firmware-test
	sh tests/run_gsp_host_test.sh
	sh tests/run_vbe_host_test.sh
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I. -o /tmp/nvx-vbios-test tests/vbios_test.c nvx_vbios.c
	ASAN_OPTIONS=detect_leaks=0 /tmp/nvx-vbios-test

HOSTCC ?= cc

.PHONY: package package-full package-probe full probe
package: all
	cp nvxmp.sys ../dist/nvxmp.sys
	genisoimage -quiet -J -R -o ../nvx_driver.iso ../dist

# Keep the known-good VBE package separate from the experimental GSP build.
full: EXPERIMENTAL_GSP=1
full: nvxmp.sys

package-full: EXPERIMENTAL_GSP=1
package-full: all
	cp ../dist/nvxdisp.dll ../dist_full/nvxdisp.dll
	cp nvxmp.sys ../dist_full/nvxmp.sys
	genisoimage -quiet -J -R -o ../nvx_driver_full.iso ../dist_full

# This deliberately unsafe diagnostic reproduces the SEC2 0x91 result without
# being confused with the boot-safe full-development package.
probe: EXPERIMENTAL_GSP=1
probe: UNSAFE_GSP_PROBE=1
probe: nvxmp.sys

package-probe: EXPERIMENTAL_GSP=1
package-probe: UNSAFE_GSP_PROBE=1
package-probe: all
	mkdir -p ../dist_probe
	cp ../dist_full/nvx.inf ../dist_full/gsp_ga104.bin ../dist_probe/
	cp ../dist/nvxdisp.dll ../dist_probe/nvxdisp.dll
	cp nvxmp.sys ../dist_probe/nvxmp.sys
	genisoimage -quiet -J -R -o ../nvx_driver_probe.iso ../dist_probe
