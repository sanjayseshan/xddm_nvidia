#include "nvx_nouveau_bo.h"

#include <stdlib.h>
#include <string.h>

int
NvxNouveauBoCreate(NVX_WINSYS_DEVICE *device,
                   const NVX_3D_ALLOC_BUFFER *description,
                   uint64_t client_token, NVX_NOUVEAU_BO *bo)
{
   NVX_3D_ALLOC_BUFFER request;

   if (!bo)
      return 0;

   memset(bo, 0, sizeof(*bo));
   if (!device || !description || !client_token ||
       client_token != device->ClientToken ||
       description->Bytes > device->caps.MaxBufferBytes)
      return 0;

   request = *description;
   request.Size = sizeof(request);
   request.Version = NVX_3D_ABI_VERSION;
   request.BufferId = 0;
   request.GpuVa = 0;
   request.ClientToken = client_token;
   if (!NvxWinsysAllocBuffer(device, &request, &bo->allocation))
      return 0;

   bo->device = device;
   bo->references = 1;
   return 1;
}

void
NvxNouveauBoReference(NVX_NOUVEAU_BO *bo)
{
   if (bo && bo->references && bo->references != UINT32_MAX)
      bo->references++;
}

int
NvxNouveauBoMarkUsed(NVX_NOUVEAU_BO *bo, uint64_t fence_value)
{
   if (!bo || !bo->references || !fence_value ||
       fence_value < bo->last_fence)
      return 0;
   bo->last_fence = fence_value;
   return 1;
}

int
NvxNouveauBoGetAddress(const NVX_NOUVEAU_BO *bo, uint64_t offset,
                      uint64_t bytes, uint32_t access, uint64_t *gpu_va)
{
   uint64_t end;

   if (gpu_va)
      *gpu_va = 0;
   if (!bo || !bo->references || !gpu_va || !bytes || !access ||
       (access & ~(NVX_NOUVEAU_BO_READ | NVX_NOUVEAU_BO_WRITE)) ||
       !bo->allocation.BufferId || !bo->allocation.GpuVa ||
       offset > bo->allocation.Bytes ||
       bytes > (uint64_t)bo->allocation.Bytes - offset ||
       bo->allocation.GpuVa > UINT64_MAX - offset)
      return 0;

   end = bo->allocation.GpuVa + offset;
   if (end > UINT64_MAX - bytes)
      return 0;

   *gpu_va = end;
   return 1;
}

int
NvxNouveauBoBuildRelocation(const NVX_NOUVEAU_BO *bo,
                           const uint32_t *command_dwords,
                           uint32_t command_count, uint32_t command_word,
                           uint64_t offset, uint64_t bytes, uint32_t access,
                           NVX_NOUVEAU_RELOCATION *relocation)
{
   NVX_NOUVEAU_RELOCATION result;

   if (relocation)
      memset(relocation, 0, sizeof(*relocation));
   if (!command_dwords || !command_count || command_word >= command_count ||
       !relocation || !bo || !bo->references)
      return 0;

   memset(&result, 0, sizeof(result));
   result.CommandWord = command_word;
   result.Access = access;
   result.BufferId = bo->allocation.BufferId;
   result.BufferOffset = offset;
   result.Bytes = bytes;
   if (!Nvx3dValidateRelocation(&result, command_count, &bo->allocation))
      return 0;
   *relocation = result;
   return 1;
}

void *
NvxNouveauBoMap(NVX_NOUVEAU_BO *bo, uint32_t access)
{
   if (!bo || !bo->references || !access ||
       (access & ~(NVX_NOUVEAU_BO_READ | NVX_NOUVEAU_BO_WRITE)) ||
       !bo->allocation.Bytes)
      return NULL;

   if (bo->map_count) {
      /* Widen the access flags if the caller requests permissions the
       * current mapping does not have. The shadow is an ordinary malloc'd
       * CPU buffer, so READ and WRITE are always physically possible once
       * the shadow exists. Widening avoids failing a diagnostic READ map
       * on a BO that Mesa already holds mapped WRITE-only. */
      bo->map_access |= access;
      bo->map_count++;
      return bo->shadow;
   }

   if (bo->last_fence &&
       !NvxWinsysWaitFence(bo->device, bo->last_fence, 5000))
      return NULL;

   bo->shadow = (uint8_t *)malloc(bo->allocation.Bytes);
   if (!bo->shadow)
      return NULL;

   /* Nouveau BO allocation leaves contents undefined. A never-used BO has
    * nothing to preserve, so start its shadow at zero instead of forcing a
    * synchronous CE readback (the NVX bridge copies through 4 KiB windows).
    * Once the BO has been uploaded or used by the GPU, preserve its contents
    * with a readback before allowing a partial CPU update. */
   if (bo->contents_valid || bo->last_fence) {
      if (!NvxWinsysDownloadBuffer(bo->device, bo->allocation.BufferId,
                                   bo->allocation.Bytes, 0, bo->shadow,
                                   bo->allocation.Bytes)) {
         free(bo->shadow);
         bo->shadow = NULL;
         return NULL;
      }
   } else {
      memset(bo->shadow, 0, bo->allocation.Bytes);
   }

   bo->map_access = access;
   bo->map_count = 1;
   return bo->shadow;
}

int
NvxNouveauBoFlush(NVX_NOUVEAU_BO *bo)
{
   if (!bo || !bo->references)
      return 0;
   if (!bo->map_count)
      return 1;
   if (!bo->shadow)
      return 0;
   if (!(bo->map_access & NVX_NOUVEAU_BO_WRITE))
      return 1;
   if (!NvxWinsysUploadBuffer(bo->device, bo->allocation.BufferId,
                              bo->allocation.Bytes, 0, bo->shadow,
                              bo->allocation.Bytes))
      return 0;
   bo->contents_valid = 1;
   return 1;
}

int
NvxNouveauBoUnmap(NVX_NOUVEAU_BO *bo)
{
   int result = 1;

   if (!bo || !bo->map_count || !bo->shadow)
      return 0;
   if (--bo->map_count)
      return 1;

   if (bo->map_access & NVX_NOUVEAU_BO_WRITE) {
      if (!NvxWinsysUploadBuffer(bo->device, bo->allocation.BufferId,
                                 bo->allocation.Bytes, 0, bo->shadow,
                                 bo->allocation.Bytes))
         result = 0;
      else
         bo->contents_valid = 1;
   }

   free(bo->shadow);
   bo->shadow = NULL;
   bo->map_access = 0;
   return result;
}

int
NvxNouveauBoDestroy(NVX_NOUVEAU_BO *bo)
{
   if (!bo || !bo->references)
      return 0;
   if (--bo->references)
      return 1;

   if (bo->map_count) {
      bo->references = 1;
      return 0;
   }
   if (bo->last_fence &&
       !NvxWinsysWaitFence(bo->device, bo->last_fence, 5000)) {
      bo->references = 1;
      return 0;
   }
   if (!NvxWinsysFreeBuffer(bo->device, bo->allocation.BufferId)) {
      bo->references = 1;
      return 0;
   }
   memset(bo, 0, sizeof(*bo));
   return 1;
}
