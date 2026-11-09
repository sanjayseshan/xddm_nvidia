CC = x86_64-w64-mingw32-gcc
CC32 = i686-w64-mingw32-gcc
EXPERIMENTAL_GSP ?= 0
UNSAFE_GSP_PROBE ?= 0
NVX_ENABLE_GR_CHANNEL_PROBE ?= 0
NVX_ENABLE_GR_PROMOTE_PROBE ?= 0
NVX_ENABLE_GR_SUBMISSION_PROBE ?= 0
OPEN_GPU_DIR = ../open-gpu-kernel-modules
CFLAGS = -DNVX_ENABLE_EXPERIMENTAL_GSP=$(EXPERIMENTAL_GSP) -DNVX_ENABLE_UNSAFE_GSP_PROBE=$(UNSAFE_GSP_PROBE) -DNVX_ENABLE_GR_CHANNEL_PROBE=$(NVX_ENABLE_GR_CHANNEL_PROBE) -DNVX_ENABLE_GR_PROMOTE_PROBE=$(NVX_ENABLE_GR_PROMOTE_PROBE) -DNVX_ENABLE_GR_SUBMISSION_PROBE=$(NVX_ENABLE_GR_SUBMISSION_PROBE) -Wall -Wextra -O2 -nostdlib -m64 -fno-builtin -fno-stack-protector -I. -Iinclude -I$(OPEN_GPU_DIR)/src/common/sdk/nvidia/inc -I$(OPEN_GPU_DIR)/src/common/inc -I$(OPEN_GPU_DIR)/src/nvidia/arch/nvalloc/common/inc -I$(OPEN_GPU_DIR)/src/common/shared/msgq/inc
CFLAGS32 = -DNVX_ENABLE_EXPERIMENTAL_GSP=$(EXPERIMENTAL_GSP) -DNVX_ENABLE_UNSAFE_GSP_PROBE=$(UNSAFE_GSP_PROBE) -DNVX_ENABLE_GR_CHANNEL_PROBE=$(NVX_ENABLE_GR_CHANNEL_PROBE) -DNVX_ENABLE_GR_PROMOTE_PROBE=$(NVX_ENABLE_GR_PROMOTE_PROBE) -DNVX_ENABLE_GR_SUBMISSION_PROBE=$(NVX_ENABLE_GR_SUBMISSION_PROBE) -Wall -Wextra -O2 -nostdlib -m32 -fno-builtin -fno-stack-protector -I. -Iinclude -I$(OPEN_GPU_DIR)/src/common/sdk/nvidia/inc -I$(OPEN_GPU_DIR)/src/common/inc -I$(OPEN_GPU_DIR)/src/nvidia/arch/nvalloc/common/inc -I$(OPEN_GPU_DIR)/src/common/shared/msgq/inc
LDFLAGS = -shared -Wl,--entry=DriverEntry -Wl,--subsystem,native:5.2 -nostartfiles -Wl,--image-base,0x10000 -Wl,--file-alignment,0x200 -Wl,--section-alignment,0x1000 -L. -lvideoprt -lntoskrnl
LDFLAGS32 = -shared -Wl,--entry=_DriverEntry@8 -Wl,--subsystem,native:5.1 -nostartfiles -Wl,--image-base,0x10000 -Wl,--file-alignment,0x200 -Wl,--section-alignment,0x1000 -lvideoprt -lntoskrnl -lgcc

all: nvxmp.sys nvxmp32.sys

.PHONY: FORCE
FORCE:

nvxmp.sys: FORCE
nvxmp32.sys: FORCE

gsp_blob.o: ../dist_full/gsp_ga104.bin
	cd .. && x86_64-w64-mingw32-objcopy -I binary -O pe-x86-64 -B i386:x86-64 dist_full/gsp_ga104.bin src_driver/gsp_blob.o

gsp_blob32.o: ../dist_full/gsp_ga104.bin
	cd .. && i686-w64-mingw32-objcopy -I binary -O pe-i386 -B i386 dist_full/gsp_ga104.bin src_driver/gsp_blob32.o
	i686-w64-mingw32-objcopy --redefine-sym _binary_dist_full_gsp_ga104_bin_start=__binary_dist_full_gsp_ga104_bin_start --redefine-sym _binary_dist_full_gsp_ga104_bin_end=__binary_dist_full_gsp_ga104_bin_end gsp_blob32.o

nvxmp.sys: nvxmp.c nvx_gsp.c nvx_gr_context.c nvx_gr_context.h nvx_mmu.c nvx_mmu.h nvx_vbe.c nvx_vbe.h nvx_fw.c nvx_fw.h nvx_vbios.c nvx_vbios.h nvx_gsp.h nvx_openrm_shim.c nvx_openrm_shim.h miniport.h bootloader_blob.h booter_load_blob.h gsp_blob.o nvxmp.def libvideoprt.a $(OPEN_GPU_DIR)/src/common/shared/msgq/msgq.c
	$(CC) $(CFLAGS) -o $@ nvxmp.c nvx_gsp.c nvx_gr_context.c nvx_mmu.c nvx_vbe.c nvx_fw.c nvx_vbios.c nvx_rpc.c nvx_seq.c nvx_openrm_shim.c $(OPEN_GPU_DIR)/src/common/shared/msgq/msgq.c gsp_blob.o nvxmp.def $(LDFLAGS)

nvxmp32.sys: nvxmp.c nvx_gsp.c nvx_gr_context.c nvx_gr_context.h nvx_mmu.c nvx_mmu.h nvx_vbe.c nvx_vbe.h nvx_fw.c nvx_fw.h nvx_vbios.c nvx_vbios.h nvx_gsp.h nvx_openrm_shim.c nvx_openrm_shim.h miniport.h bootloader_blob.h booter_load_blob.h gsp_blob32.o $(OPEN_GPU_DIR)/src/common/shared/msgq/msgq.c
	$(CC32) $(CFLAGS32) -o $@ nvxmp.c nvx_gsp.c nvx_gr_context.c nvx_mmu.c nvx_vbe.c nvx_fw.c nvx_vbios.c nvx_rpc.c nvx_seq.c nvx_openrm_shim.c $(OPEN_GPU_DIR)/src/common/shared/msgq/msgq.c gsp_blob32.o $(LDFLAGS32)

libvideoprt.a: videoprt.def
	x86_64-w64-mingw32-dlltool -d videoprt.def -l libvideoprt.a

clean:
	rm -f nvxmp.sys nvxmp32.sys libvideoprt.a gsp_blob.o gsp_blob32.o

.PHONY: check
check:
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I. -o /tmp/nvx-gr-context-test tests/gr_context_test.c nvx_gr_context.c
	ASAN_OPTIONS=detect_leaks=0 /tmp/nvx-gr-context-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I. -o /tmp/nvx-display-test tests/display_test.c
	ASAN_OPTIONS=detect_leaks=0 /tmp/nvx-display-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I. -o /tmp/nvx-mmu-test tests/mmu_test.c nvx_mmu.c
	ASAN_OPTIONS=detect_leaks=0 /tmp/nvx-mmu-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I. -o /tmp/nvx-seq-test tests/seq_test.c nvx_seq.c
	ASAN_OPTIONS=detect_leaks=0 /tmp/nvx-seq-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I. -o /tmp/nvx-rpc-test tests/rpc_test.c nvx_rpc.c
	ASAN_OPTIONS=detect_leaks=0 /tmp/nvx-rpc-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I. -o /tmp/nvx-firmware-test tests/firmware_test.c nvx_fw.c
	ASAN_OPTIONS=detect_leaks=0 /tmp/nvx-firmware-test
	sh tests/run_gsp_host_test.sh
	sh tests/run_vbe_host_test.sh
	sh tests/run_openrm_shim_test.sh
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I. -Iinclude -I$(OPEN_GPU_DIR)/src/common/sdk/nvidia/inc -I$(OPEN_GPU_DIR)/src/common/inc -I$(OPEN_GPU_DIR)/src/nvidia/arch/nvalloc/common/inc -I$(OPEN_GPU_DIR)/src/common/shared/msgq/inc -o /tmp/nvx-vbios-test tests/vbios_test.c nvx_vbios.c
	ASAN_OPTIONS=detect_leaks=0 /tmp/nvx-vbios-test

HOSTCC ?= cc

.PHONY: package package-full package-probe full full-gr-probe probe
package: all
	cp nvxmp.sys ../dist/nvxmp.sys
	genisoimage -quiet -J -R -o ../nvx_driver.iso ../dist

# Keep the known-good VBE package separate from the experimental GSP build.
full: EXPERIMENTAL_GSP=1
full: nvxmp.sys

full32: EXPERIMENTAL_GSP=1
full32: nvxmp32.sys

package-full: EXPERIMENTAL_GSP=1
package-full: all
	[ ! -f ../dist/nvxdisp.dll ] || cp ../dist/nvxdisp.dll ../dist_full/nvxdisp.dll
	cp nvxmp.sys ../dist_full/nvxmp.sys
	genisoimage -quiet -J -R -o ../nvx_driver_full.iso ../dist_full

# This deliberately unsafe diagnostic reproduces the SEC2 0x91 result without
# being confused with the boot-safe full-development package.
probe: EXPERIMENTAL_GSP=1
probe: UNSAFE_GSP_PROBE=1
probe: nvxmp.sys

# Opt-in RM allocation diagnostic. It does not submit rendering work and is
# deliberately excluded from normal full builds until tested on the XP guest.
full-gr-probe: EXPERIMENTAL_GSP=1
full-gr-probe: NVX_ENABLE_GR_CHANNEL_PROBE=1
full-gr-probe: nvxmp.sys

package-probe: EXPERIMENTAL_GSP=1
package-probe: UNSAFE_GSP_PROBE=1
package-probe: all
	mkdir -p ../dist_probe
	cp ../dist_full/nvx.inf ../dist_full/gsp_ga104.bin ../dist_probe/
	cp ../dist/nvxdisp.dll ../dist_probe/nvxdisp.dll
	cp nvxmp.sys ../dist_probe/nvxmp.sys
	genisoimage -quiet -J -R -o ../nvx_driver_probe.iso ../dist_probe
