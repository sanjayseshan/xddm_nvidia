#ifndef NVX_WINSYS_H
#define NVX_WINSYS_H

#include <stdint.h>

#include "../src_driver/nvx_3d_abi.h"

typedef int (*NVX_WINSYS_ESCAPE)(void *opaque, uint32_t escape,
                                 uint32_t input_bytes, const void *input,
                                 uint32_t output_bytes, void *output);

typedef struct NVX_WINSYS_DEVICE {
   NVX_WINSYS_ESCAPE escape;
   void *opaque;
   NVX_3D_QUERY_CAPS caps;
} NVX_WINSYS_DEVICE;

int NvxWinsysInit(NVX_WINSYS_DEVICE *device, NVX_WINSYS_ESCAPE escape,
                  void *opaque);
int NvxWinsysAllocBuffer(NVX_WINSYS_DEVICE *device,
                         const NVX_3D_ALLOC_BUFFER *request,
                         NVX_3D_ALLOC_BUFFER *result);
int NvxWinsysFreeBuffer(NVX_WINSYS_DEVICE *device, uint64_t buffer_id);
int NvxWinsysUploadBuffer(NVX_WINSYS_DEVICE *device, uint64_t buffer_id,
                          uint32_t allocated_bytes, uint32_t offset,
                          const void *data, uint32_t bytes);
int NvxWinsysDownloadBuffer(NVX_WINSYS_DEVICE *device, uint64_t buffer_id,
                            uint32_t allocated_bytes, uint32_t offset,
                            void *data, uint32_t bytes);
int NvxWinsysWaitFence(NVX_WINSYS_DEVICE *device, uint64_t fence_value,
                       uint32_t timeout_ms);
int NvxWinsysValidateBatch(NVX_WINSYS_DEVICE *device,
                           const void *batch, uint32_t batch_bytes);

#endif
