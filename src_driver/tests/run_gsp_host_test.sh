#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d /tmp/nvx-gsp-test.XXXXXX)
trap 'rm -rf "$build_dir"' EXIT
cp -r include nvx_display.h nvx_mmu.c nvx_mmu.h nvx_gsp.c nvx_ce.c nvx_disp_rpc.c nvx_gsp_boot.c nvx_fw.c nvx_fw.h nvx_rpc.c nvx_rpc.h nvx_seq.c nvx_seq.h nvx_openrm_shim.h nvx_openrm_shim.c bootloader_blob.h booter_load_blob.h tests/gsp_mock_test.c tests/host_videoport.h "$build_dir/"
cp tests/host_videoport.h "$build_dir/miniport.h"
sed 's/#include "miniport.h"/#include "host_videoport.h"/' nvx_gsp.h > "$build_dir/nvx_gsp.h"
# Preserve firmware linker symbol names used by the production image.
ld -r -b binary ../dist_full/gsp_ga104.bin -o "$build_dir/blob.o"
objcopy --redefine-sym _binary____dist_full_gsp_ga104_bin_start=_binary_dist_full_gsp_ga104_bin_start --redefine-sym _binary____dist_full_gsp_ga104_bin_end=_binary_dist_full_gsp_ga104_bin_end "$build_dir/blob.o"
${HOSTCC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -Wl,-z,noexecstack -I"$build_dir" -I"$build_dir/include" -I$(pwd)/../open-gpu-kernel-modules/src/common/sdk/nvidia/inc -I$(pwd)/../open-gpu-kernel-modules/src/common/inc -I$(pwd)/../open-gpu-kernel-modules/src/nvidia/arch/nvalloc/common/inc -I$(pwd)/../open-gpu-kernel-modules/src/common/shared/msgq/inc -o "$build_dir/test" "$build_dir/gsp_mock_test.c" "$build_dir/nvx_gsp.c" "$build_dir/nvx_mmu.c" "$build_dir/nvx_fw.c" "$build_dir/nvx_rpc.c" "$build_dir/nvx_seq.c" "$build_dir/nvx_openrm_shim.c" "$(pwd)/../open-gpu-kernel-modules/src/common/shared/msgq/msgq.c" "$build_dir/blob.o"
ASAN_OPTIONS=detect_leaks=0 "$build_dir/test"
