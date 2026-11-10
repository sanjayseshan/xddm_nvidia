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
    sph_imap_attr0 = 6
    baseline_imap0 = struct.unpack_from("<I", blob, sph_imap_attr0 * 4)[0]
    if baseline_imap0 & 0x0f != 0x03:
        raise SystemExit(
            f"unexpected baseline SPH attr0 IMAP mask: 0x{baseline_imap0:08x}"
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
    # Preserve the vendor position stores and its aligned generic attribute-8
    # ALD.64 R8,a[0x100]. Append AST.64 a[0x78],R8 before EXIT so R8/R9 become
    # clip Z/W while the original varying output remains intact.
    exit_offset = VS_OFFSET + 0x180
    branch_offset = VS_OFFSET + 0x190
    exit_instruction = bytes(blob[exit_offset:exit_offset + 16])
    branch_instruction = bytes(blob[branch_offset:branch_offset + 16])
    struct.pack_into("<QQ", blob, exit_offset,
                     0x00007808ff007322, 0x001fee00000004ff)
    blob[branch_offset:branch_offset + 16] = exit_instruction
    blob[branch_offset + 16:branch_offset + 32] = branch_instruction
    struct.pack_into("<I", blob, sph_omap_system_values_ab * 4,
                     baseline_omap | sph_position_z)
    depth_code = blob[VS_OFFSET:VS_OFFSET + VS_SIZE]
    depth_instructions = {
        struct.unpack_from("<QQ", depth_code, offset)
        for offset in range(0, VS_SIZE, 16)
    }
    depth_zw_load = (0x000100ffff087321, 0x000e220000000400)
    depth_zw_store = (0x00007808ff007322, 0x001fee00000004ff)
    if (depth_zw_load not in depth_instructions or
            depth_zw_store not in depth_instructions):
        raise SystemExit("generated depth load/store encodings did not validate")
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
    print("Ampere depth VS audit: SPH XYZW declaration and generic-attribute Z/W output validated")
    print(f"Ampere depth VS audit: candidate disassembly input written to {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
