#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d /tmp/nvx-openrm-test.XXXXXX)
trap 'rm -rf "$build_dir"' EXIT
cp -r include nvx_display.h nvx_gr_context.h nvx_gsp.h nvx_openrm_shim.h nvx_openrm_shim.c tests/openrm_shim_test.c "$build_dir/"
cp tests/host_videoport.h "$build_dir/miniport.h"
cp tests/host_videoport.h "$build_dir/host_videoport.h"
${HOSTCC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I"$build_dir" -I"$build_dir/include" -I$(pwd)/../open-gpu-kernel-modules/src/common/sdk/nvidia/inc -I$(pwd)/../open-gpu-kernel-modules/src/common/inc -I$(pwd)/../open-gpu-kernel-modules/src/nvidia/arch/nvalloc/common/inc -I$(pwd)/../open-gpu-kernel-modules/src/common/shared/msgq/inc -o "$build_dir/test" "$build_dir/openrm_shim_test.c" "$build_dir/nvx_openrm_shim.c"
ASAN_OPTIONS=detect_leaks=0 "$build_dir/test"
