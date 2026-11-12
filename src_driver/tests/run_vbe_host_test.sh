#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d /tmp/nvx-vbe-test.XXXXXX)
trap 'rm -rf "$build_dir"' EXIT
cp -r include nvx_display.h nvx_gr_context.h nvx_vbe.c nvx_vbe.h tests/vbe_mock_test.c tests/host_videoport.h "$build_dir/"
sed 's/#include "miniport.h"/#include "host_videoport.h"/' nvx_gsp.h > "$build_dir/nvx_gsp.h"
${HOSTCC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I"$build_dir" -I"$build_dir/include" -I$(pwd)/../open-gpu-kernel-modules/src/common/sdk/nvidia/inc -I$(pwd)/../open-gpu-kernel-modules/src/common/inc -I$(pwd)/../open-gpu-kernel-modules/src/nvidia/arch/nvalloc/common/inc -I$(pwd)/../open-gpu-kernel-modules/src/common/shared/msgq/inc -o "$build_dir/test" "$build_dir/vbe_mock_test.c" "$build_dir/nvx_vbe.c"
ASAN_OPTIONS=detect_leaks=0 "$build_dir/test"
