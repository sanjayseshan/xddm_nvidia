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
#endif
