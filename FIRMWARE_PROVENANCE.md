# NVIDIA 535.113.01 firmware provenance

Verified: November 1, 2026

The experimental GA104 GSP path is intentionally pinned to NVIDIA release
535.113.01. Do not mix blobs or ABI definitions from another driver release.

## Official inputs

- NVIDIA Linux x86_64 535.113.01, no-compat32 package:
  `https://download.nvidia.com/XFree86/Linux-x86_64/535.113.01/NVIDIA-Linux-x86_64-535.113.01-no-compat32.run`
- NVIDIA's published checksum file:
  `https://download.nvidia.com/XFree86/Linux-x86_64/535.113.01/sha256sum`
- Matching open kernel module tag:
  `https://github.com/NVIDIA/open-gpu-kernel-modules/tree/535.113.01`
- NVIDIA's Nouveau firmware extraction script:
  `nouveau/extract-firmware-nouveau.py` in that tag.

The downloaded installer is kept under `vendor/` locally and is ignored by
Git because it is about 270 MB and can be reproduced from the URL above.
`NVIDIA-Linux-x86_64-535.113.01-no-compat32.run --check` passed, and its
SHA-256 matched NVIDIA's checksum file:

```text
705ea3e47f31d49109f9d48e8057bcffaec0cfae70f62b7636a1d4e1d8cd4e78
```

## Verified project artifacts

The official package's `firmware/gsp_ga10x.bin` is byte-identical to the
existing `dist_full/gsp_ga104.bin`:

```text
e30231ec0d317021770e16c81ae2c0553f728d0344248827582f78132c9447d4
```

Running NVIDIA's matching extraction script against the 535.113.01 OpenRM
tree produced the following GA102/GA10x SEC2 artifacts:

```text
076d2a0675131ad8a8de215c611ce4271289adcc03e2cf39995f9b29079e2136  bootloader
3ab46bf4c70d72e8fc89d98e6bdc9b83a61954a586629d26b8b5653a620237e0  booter load
09240c83595856a0fe70d2ab3ec7325455cd8adfc48ec7b47f6fced1e5431beb  booter unload
```

The first two are byte-identical to
`src_driver/bootloader_535_113_01.bin` and
`src_driver/booter_load_535_113_01.bin`. The previously missing official
unload artifact is now stored as
`src_driver/booter_unload_535_113_01.bin` for the later shutdown path. It is
not executed by v35.

## What cannot be replaced by an Internet download

FWSEC/FRTS is selected from the GPU board's own VBIOS and includes descriptor,
signature, and board-ROM layout information. It is not the generic GSP-RM
image in NVIDIA's Linux installer. The target is an EVGA RTX 3070 XC3 Ultra
LHR (device 10de:2488, VBIOS 94.04.46.40.60), so a ROM from another board or a
third-party modified ROM must not be substituted or flashed.

The captured sysfs ROM is only 157696 bytes, while its BIT Falcon-table pointer
is 0x72383, outside that capture. The live v35 PROM diagnostic shows valid PCI
images at offsets 0 and 0xfe00, followed by an NVIDIA private expansion image
at 0x26800 beginning with `NV`. The next safe step is to parse that private
image directly from the read-only BAR0 PROM aperture, locate and validate the
board FWSEC descriptor, and log it. Falcon execution stays disabled until that
read-only stage passes.
