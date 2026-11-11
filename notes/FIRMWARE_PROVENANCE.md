# NVIDIA firmware provenance

The confirmed driver build uses GSP-RM firmware from NVIDIA Linux x86-64
535.113.01, no-compat32. Firmware from another driver release must not be
substituted without updating and validating the matching firmware ABI.

Official package:

- NVIDIA Linux x86-64 535.113.01:
  <https://download.nvidia.com/XFree86/Linux-x86_64/535.113.01/NVIDIA-Linux-x86_64-535.113.01-no-compat32.run>
- NVIDIA checksum file:
  <https://download.nvidia.com/XFree86/Linux-x86_64/535.113.01/sha256sum>
- Matching Open GPU Kernel Modules tag:
  <https://github.com/NVIDIA/open-gpu-kernel-modules/tree/535.113.01>

Verified SHA-256 values:

```text
NVIDIA-Linux-x86_64-535.113.01-no-compat32.run
705ea3e47f31d49109f9d48e8057bcffaec0cfae70f62b7636a1d4e1d8cd4e78

../dist_full/gsp_ga104.bin
e30231ec0d317021770e16c81ae2c0553f728d0344248827582f78132c9447d4

../src_driver/bootloader_535_113_01.bin
076d2a0675131ad8a8de215c611ce4271289adcc03e2cf39995f9b29079e2136

../src_driver/booter_load_535_113_01.bin
3ab46bf4c70d72e8fc89d98e6bdc9b83a61954a586629d26b8b5653a620237e0

../src_driver/booter_unload_535_113_01.bin
09240c83595856a0fe70d2ab3ec7325455cd8adfc48ec7b47f6fced1e5431beb
```

The SEC2 bootloader artifacts above were extracted using NVIDIA's matching
firmware extraction script from the 535.113.01 OpenRM source. The project
targets an EVGA RTX 3070 XC3 Ultra LHR; board-specific VBIOS/FWSEC data must
come from the target GPU itself. Do not flash or substitute a VBIOS from
another board.

The firmware remains NVIDIA intellectual property and is subject to NVIDIA's
license terms. This provenance record does not grant redistribution rights.
