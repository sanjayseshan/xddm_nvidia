CC = x86_64-w64-mingw32-gcc
EXPERIMENTAL_GSP ?= 0
CFLAGS = -DNVX_ENABLE_EXPERIMENTAL_GSP=$(EXPERIMENTAL_GSP) -Wall -Wextra -O2 -nostdlib -m64 -fno-builtin -fno-stack-protector -I.
LDFLAGS = -shared -Wl,--entry=DriverEntry -Wl,--subsystem,native:5.2 -nostartfiles -Wl,--image-base,0x10000 -Wl,--file-alignment,0x200 -Wl,--section-alignment,0x1000 -L. -lvideoprt

all: nvxmp.sys

.PHONY: FORCE
FORCE:

nvxmp.sys: FORCE

gsp_blob.o: ../dist_full/gsp_ga104.bin
	cd .. && x86_64-w64-mingw32-objcopy -I binary -O pe-x86-64 -B i386:x86-64 dist_full/gsp_ga104.bin src_driver/gsp_blob.o

nvxmp.sys: nvxmp.c nvx_gsp.c nvx_vbe.c nvx_vbe.h nvx_fw.c nvx_fw.h nvx_gsp.h miniport.h bootloader_blob.h booter_load_blob.h gsp_blob.o nvxmp.def libvideoprt.a
	$(CC) $(CFLAGS) -o $@ nvxmp.c nvx_gsp.c nvx_vbe.c nvx_fw.c gsp_blob.o nvxmp.def $(LDFLAGS)

libvideoprt.a: videoprt.def
	x86_64-w64-mingw32-dlltool -d videoprt.def -l libvideoprt.a

clean:
	rm -f nvxmp.sys libvideoprt.a gsp_blob.o

.PHONY: check
check:
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I. -o /tmp/nvx-firmware-test tests/firmware_test.c nvx_fw.c
	ASAN_OPTIONS=detect_leaks=0 /tmp/nvx-firmware-test
	sh tests/run_gsp_host_test.sh
	sh tests/run_vbe_host_test.sh

HOSTCC ?= cc

.PHONY: package package-full full
package: all
	cp nvxmp.sys ../dist/nvxmp.sys
	genisoimage -quiet -J -R -o ../nvx_driver.iso ../dist

# Keep the known-good VBE package separate from the experimental GSP build.
full: EXPERIMENTAL_GSP=1
full: nvxmp.sys

package-full: EXPERIMENTAL_GSP=1
package-full: all
	cp nvxmp.sys ../dist_full/nvxmp.sys
	genisoimage -quiet -J -R -o ../nvx_driver_full.iso ../dist_full
