#ifndef NVX_GR_CONTEXT_H
#define NVX_GR_CONTEXT_H
#include <stdint.h>

/* Indices are NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID;
   buffer IDs are NV2080_CTRL_GPU_PROMOTE_CTX_BUFFER_ID. */
#define NVX_GR_REQUIREMENT_COUNT 25U
#define NVX_GR_MAX_BUFFERS 12U
typedef struct { uint32_t size, alignment; } NVX_GR_REQUIREMENT;
typedef struct { uint32_t id, alignment; uint64_t offset, size; } NVX_GR_BUFFER;
typedef struct { uint32_t count; uint64_t bytes; NVX_GR_BUFFER buffers[NVX_GR_MAX_BUFFERS]; } NVX_GR_LAYOUT;

/* Build a bounded, page-isolated layout; never map or initialize GPU memory. */
int NvxGrBuildLayout(const NVX_GR_REQUIREMENT *, uint32_t, int, uint64_t, NVX_GR_LAYOUT *);
/* Encode 4KB coherent system-memory leaves into caller-owned PTEs. */
int NvxGrBuildPtes(uint64_t *, uint32_t, uint64_t, uint64_t, int);

/* Reserved Gallium/Nouveau GART window: 115 2 MiB slices immediately below
 * the existing VRAM aperture at 0x20000000. One 4 KiB small-page table maps
 * each slice. */
#define NVX_GART_GPU_VA_BASE UINT64_C(0x11a00000)
#define NVX_GART_SLICE_BYTES UINT32_C(0x00200000)
#define NVX_GART_SLICE_COUNT 115U
#define NVX_GART_PTES_PER_SLICE 512U
#define NVX_GART_PAGE_BYTES 4096U
#define NVX_GART_MIN_VA_ALIGNMENT 0x10000U
#define NVX_GART_PAGE_COUNT (NVX_GART_SLICE_COUNT * NVX_GART_PTES_PER_SLICE)
#define NVX_GART_TOTAL_BYTES ((uint64_t)NVX_GART_SLICE_COUNT * NVX_GART_SLICE_BYTES)
#define NVX_GART_BITMAP_WORDS ((NVX_GART_PAGE_COUNT + 31U) / 32U)
typedef struct NVX_GART_HEAP {
    uint32_t used[NVX_GART_BITMAP_WORDS];
} NVX_GART_HEAP;
int NvxGartReserve(NVX_GART_HEAP *, uint64_t, uint32_t, uint32_t *, uint32_t *, uint64_t *);
int NvxGartRelease(NVX_GART_HEAP *, uint32_t, uint32_t);
int NvxGartBuildPtes(uint64_t *, uint32_t, uint32_t, uint32_t,
                     uint64_t, uint64_t, int);
#endif
