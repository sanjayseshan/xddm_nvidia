#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

OBJ_DIR="$SCRIPT_DIR/obj_vxd"
mkdir -p "$OBJ_DIR"
mkdir -p "$ROOT_DIR/dist_win98"

CC="i686-w64-mingw32-gcc"
LD="i686-w64-mingw32-ld"
OBJCOPY="i686-w64-mingw32-objcopy"
LIBGCC="/usr/lib/gcc/i686-w64-mingw32/12-win32/libgcc.a"

CFLAGS="-m32 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-pointer-to-int-cast -fno-pie -fno-stack-protector -falign-functions=16 -falign-labels=16 -falign-loops=16 -falign-jumps=16 -D__VXDKERNEL__ -DNVX_ENABLE_EXPERIMENTAL_GSP=1 -I$ROOT_DIR/src_driver -I$SCRIPT_DIR/shim -nostdlib -fno-builtin"

echo "==> 1. Assembling VxD entry and VMM wrappers..."
nasm -f win32 "$SCRIPT_DIR/vxd_entry.asm" -o "$OBJ_DIR/vxd_entry.o"
nasm -f win32 "$SCRIPT_DIR/shim/vmm_wrappers.asm" -o "$OBJ_DIR/vmm_wrappers.o"

echo "==> 2. Compiling VideoPort VxD shim..."
$CC $CFLAGS -c "$SCRIPT_DIR/shim/videoprt_shim.c" -o "$OBJ_DIR/videoprt_shim.o"

echo "==> 3. Compiling core driver modules..."
$CC $CFLAGS -c "$ROOT_DIR/src_driver/nvx_fw.c" -o "$OBJ_DIR/nvx_fw.o"
$CC $CFLAGS -c "$ROOT_DIR/src_driver/nvx_rpc.c" -o "$OBJ_DIR/nvx_rpc.o"
$CC $CFLAGS -c "$ROOT_DIR/src_driver/nvx_seq.c" -o "$OBJ_DIR/nvx_seq.o"
$CC $CFLAGS -c "$ROOT_DIR/src_driver/nvx_mmu.c" -o "$OBJ_DIR/nvx_mmu.o"
$CC $CFLAGS -c "$ROOT_DIR/src_driver/nvx_vbios.c" -o "$OBJ_DIR/nvx_vbios.o"
$CC $CFLAGS -c "$ROOT_DIR/src_driver/nvx_gsp.c" -o "$OBJ_DIR/nvx_gsp.o"
$CC $CFLAGS -c "$ROOT_DIR/src_driver/nvxmp.c" -o "$OBJ_DIR/nvxmp.o"

cat << 'EOF' > "$OBJ_DIR/vxd0.ld"
SECTIONS
{
    . = 0x00000000;
    .all : {
        *(.text.entry)
        . = ALIGN(64);
        *(.text*)
        *(.rdata*)
        *(.rodata*)
        *(.data*)
        *(.bss*)
        *(COMMON)
    }
    /DISCARD/ : {
        *(.eh_frame*)
        *(.reloc*)
        *(.comment*)
        *(.note*)
    }
}
EOF

cat << 'EOF' > "$OBJ_DIR/vxd1.ld"
SECTIONS
{
    . = 0x00010000;
    .all : {
        *(.text.entry)
        . = ALIGN(64);
        *(.text*)
        *(.rdata*)
        *(.rodata*)
        *(.data*)
        *(.bss*)
        *(COMMON)
    }
    /DISCARD/ : {
        *(.eh_frame*)
        *(.reloc*)
        *(.comment*)
        *(.note*)
    }
}
EOF

OBJECTS="$OBJ_DIR/vxd_entry.o $OBJ_DIR/vmm_wrappers.o $OBJ_DIR/videoprt_shim.o $OBJ_DIR/nvxmp.o $OBJ_DIR/nvx_gsp.o $OBJ_DIR/nvx_fw.o $OBJ_DIR/nvx_rpc.o $OBJ_DIR/nvx_seq.o $OBJ_DIR/nvx_mmu.o $OBJ_DIR/nvx_vbios.o $LIBGCC"

echo "==> 5. Linking base 0 and base 0x10000 images..."
$LD -m i386pe --image-base 0x00000000 -T "$OBJ_DIR/vxd0.ld" $OBJECTS -o "$OBJ_DIR/a0.pe"
$LD -m i386pe --image-base 0x00010000 -T "$OBJ_DIR/vxd1.ld" $OBJECTS -o "$OBJ_DIR/a1.pe"

$OBJCOPY -O binary "$OBJ_DIR/a0.pe" "$OBJ_DIR/a0.bin"
$OBJCOPY -O binary "$OBJ_DIR/a1.pe" "$OBJ_DIR/a1.bin"

echo "==> 6. Generating final 32-bit LE VxD (nvx.vxd)..."
python3 "$SCRIPT_DIR/tools/mkvxd32.py" NVX "$OBJ_DIR/a0.bin" "$OBJ_DIR/a1.bin" "$ROOT_DIR/dist_win98/nvx.vxd"

ls -lh "$ROOT_DIR/dist_win98/nvx.vxd"
echo "==> VxD build complete!"
