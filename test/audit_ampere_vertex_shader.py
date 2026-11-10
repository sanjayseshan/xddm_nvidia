#!/usr/bin/env python3
"""Validate the baseline and project-owned depth variant of Ampere head VS."""

from pathlib import Path
import struct
import sys
import tempfile


SHADER = Path(__file__).resolve().parents[1] / (
    "open-gpu-kernel-modules/src/nvidia-modeset/src/shaders/"
    "g_ampere_shaders"
)
VS_OFFSET = 0x80
VS_SIZE = 0x280
DEPTH_LOAD_OFFSET = VS_OFFSET + 0x50
DEPTH_STORE_OFFSET = VS_OFFSET + 0xb0


def main() -> int:
    data = SHADER.read_bytes()
    if len(data) < VS_OFFSET + VS_SIZE:
        raise SystemExit(f"shader blob truncated: {len(data)} bytes")
    blob = bytearray(data[:1408])
    sph_omap_system_values_ab = 13
    sph_position_xyw = (1 << 12) | (1 << 13) | (1 << 15)
    sph_position_z = 1 << 14
    baseline_omap = struct.unpack_from("<I", blob, sph_omap_system_values_ab * 4)[0]
    if baseline_omap & (sph_position_xyw | sph_position_z) != sph_position_xyw:
        raise SystemExit(
            f"unexpected baseline SPH position mask: 0x{baseline_omap:08x}"
        )
    code = blob[VS_OFFSET:VS_OFFSET + VS_SIZE]
    instructions = []
    for offset in range(0, VS_SIZE, 16):
        low, high = struct.unpack_from("<QQ", code, offset)
        instructions.append((low, high))
    # Instruction words are the encodings printed by nvdisasm for this pinned
    # upstream blob. Exact matching avoids guessing undocumented SASS fields.
    known_loads = {
        (0x000100ffff087321, 0x000e220000000400),  # ALD.64 a[0x100]
        (0x000080ffff0a7321, 0x000fe20000000000),  # ALD a[0x80]
        (0x000084ffff0b7321, 0x000fe80000000000),  # ALD a[0x84]
    }
    known_stores = {
        (0x00030008ff007322, 0x001fee00000004ff),  # AST.64 a[0x300]
        (0x00031000ff007322, 0x000fe400000004ff),  # AST.64 a[0x310]
        (0x00032002ff007322, 0x000fe200000004ff),  # AST.64 a[0x320]
        (0x00007c0cff007322, 0x004fee00000000ff),  # AST a[0x7c]
        (0x00007406ff007322, 0x000fe200000000ff),  # AST a[0x74]
        (0x00007007ff007322, 0x000fe200000000ff),  # AST a[0x70]
    }
    if not known_loads.issubset(set(instructions)):
        raise SystemExit("expected position/attribute loads are missing")
    if not known_stores.issubset(set(instructions)):
        raise SystemExit("expected clip XY/W and varying stores are missing")
    baseline_load = (0x00010cffff0c7321, 0x000eaa0000000000)
    baseline_store = (0x00007c0cff007322, 0x004fee00000000ff)
    if baseline_load not in instructions or baseline_store not in instructions:
        raise SystemExit("expected baseline position-W load/store are missing")
    if any(low == 0x0000780cff007322 for low, _ in instructions):
        raise SystemExit("upstream head-surface VS unexpectedly writes clip Z")
    # Attribute 0 (position) begins at a[0x80], so Z/W are a[0x88].
    # a[0x100]/a[0x108] belong to generic attribute 8 (texture coordinates).
    struct.pack_into("<QQ", blob, DEPTH_LOAD_OFFSET,
                     0x000088ffff0c7321, 0x000e220000000400)
    struct.pack_into("<QQ", blob, DEPTH_STORE_OFFSET,
                     0x0000780cff007322, 0x001fee00000004ff)
    struct.pack_into("<I", blob, sph_omap_system_values_ab * 4,
                     baseline_omap | sph_position_z)
    depth_code = blob[VS_OFFSET:VS_OFFSET + VS_SIZE]
    depth_instructions = {
        struct.unpack_from("<QQ", depth_code, offset)
        for offset in range(0, VS_SIZE, 16)
    }
    depth_load = (0x000088ffff0c7321, 0x000e220000000400)
    depth_store = (0x0000780cff007322, 0x001fee00000004ff)
    if depth_load not in depth_instructions or depth_store not in depth_instructions:
        raise SystemExit("generated depth load/store encodings did not validate")
    if baseline_load in depth_instructions or baseline_store in depth_instructions:
        raise SystemExit("depth variant retained a replaced baseline instruction")
    depth_omap = struct.unpack_from(
        "<I", blob, sph_omap_system_values_ab * 4
    )[0]
    if depth_omap & (sph_position_xyw | sph_position_z) != (
        sph_position_xyw | sph_position_z
    ):
        raise SystemExit("depth variant SPH does not declare clip-space Z")
    output = Path(tempfile.gettempdir()) / "nvx-ampere-depth-vs.bin"
    output.write_bytes(depth_code)
    print("Ampere head-surface VS audit: baseline declares clip XY/W, omits Z")
    print("Ampere depth VS audit: SPH Z declaration and paired Z/W instruction encodings validated")
    print(f"Ampere depth VS audit: candidate disassembly input written to {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
