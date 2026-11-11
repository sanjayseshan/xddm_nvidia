#include "nvx_nouveau_pushbuf.h"

#include <stdlib.h>
#include <string.h>

int
NvxNouveauPushbufInit(NVX_NOUVEAU_PUSHBUF *push,
                      NVX_WINSYS_DEVICE *device, uint64_t client_token)
{
   if (!push || !device || !device->escape || !device->opaque ||
       !client_token)
      return 0;
   memset(push, 0, sizeof(*push));
   push->device = device;
   push->client_token = client_token;
   return 1;
}

void
NvxNouveauPushbufReset(NVX_NOUVEAU_PUSHBUF *push)
{
   if (!push)
      return;
   push->dword_count = 0;
   push->relocation_count = 0;
}

void
NvxNouveauPushbufDestroy(NVX_NOUVEAU_PUSHBUF *push)
{
   if (!push)
      return;
   free(push->dwords);
   free(push->relocations);
   memset(push, 0, sizeof(*push));
}

int
NvxNouveauPushbufAppend(NVX_NOUVEAU_PUSHBUF *push,
                        const uint32_t *dwords, uint32_t count)
{
   uint32_t capacity;
   uint32_t *grown;

   if (!push || !push->device || !dwords || !count ||
       count > NVX_3D_MAX_PUSH_DWORDS - push->dword_count)
      return 0;
   if (count > push->dword_capacity - push->dword_count) {
      capacity = push->dword_capacity ? push->dword_capacity : 64U;
      while (capacity - push->dword_count < count) {
         if (capacity > NVX_3D_MAX_PUSH_DWORDS / 2U) {
            capacity = NVX_3D_MAX_PUSH_DWORDS;
            break;
         }
         capacity *= 2U;
      }
      if (capacity - push->dword_count < count)
         return 0;
      grown = (uint32_t *)realloc(push->dwords,
                                  (size_t)capacity * sizeof(*grown));
      if (!grown)
         return 0;
      push->dwords = grown;
      push->dword_capacity = capacity;
   }
   memcpy(push->dwords + push->dword_count, dwords,
          (size_t)count * sizeof(*dwords));
   push->dword_count += count;
   return 1;
}

int
NvxNouveauPushbufRelocate(NVX_NOUVEAU_PUSHBUF *push,
                          const NVX_NOUVEAU_BO *bo,
                          uint32_t command_word, uint64_t offset,
                          uint64_t bytes, uint32_t access)
{
   uint32_t capacity;
   NVX_NOUVEAU_RELOCATION *grown;
   NVX_NOUVEAU_RELOCATION relocation;

   if (!push || !push->device ||
       push->relocation_count >= NVX_3D_MAX_BATCH_RELOCATIONS ||
       !NvxNouveauBoBuildRelocation(bo, push->dwords, push->dword_count,
                                   command_word, offset, bytes, access,
                                   &relocation))
      return 0;

   if (push->relocation_count == push->relocation_capacity) {
      capacity = push->relocation_capacity ?
         push->relocation_capacity * 2U : 8U;
      if (capacity > NVX_3D_MAX_BATCH_RELOCATIONS)
         capacity = NVX_3D_MAX_BATCH_RELOCATIONS;
      grown = (NVX_NOUVEAU_RELOCATION *)realloc(
         push->relocations, (size_t)capacity * sizeof(*grown));
      if (!grown)
         return 0;
      push->relocations = grown;
      push->relocation_capacity = capacity;
   }
   push->relocations[push->relocation_count++] = relocation;
   return 1;
}

int
NvxNouveauPushbufValidate(NVX_NOUVEAU_PUSHBUF *push)
{
   NVX_3D_BATCH_HEADER header;
   uint64_t total_bytes;
   uint8_t *batch;
   int valid;

   if (!push || !push->device || !push->client_token || !push->dword_count ||
       push->dword_count > NVX_3D_MAX_PUSH_DWORDS ||
       push->relocation_count > NVX_3D_MAX_BATCH_RELOCATIONS)
      return 0;

   memset(&header, 0, sizeof(header));
   header.Size = sizeof(header);
   header.Version = NVX_3D_ABI_VERSION;
   header.DwordCount = push->dword_count;
   header.RelocationCount = push->relocation_count;
   header.ClientToken = push->client_token;
   total_bytes = sizeof(header) +
      (uint64_t)push->dword_count * sizeof(*push->dwords) +
      (uint64_t)push->relocation_count * sizeof(*push->relocations);
   if (total_bytes > NVX_3D_MAX_BATCH_BYTES || total_bytes > UINT32_MAX)
      return 0;

   batch = (uint8_t *)malloc((size_t)total_bytes);
   if (!batch)
      return 0;
   memcpy(batch, &header, sizeof(header));
   memcpy(batch + sizeof(header), push->dwords,
          (size_t)push->dword_count * sizeof(*push->dwords));
   if (push->relocation_count)
      memcpy(batch + sizeof(header) +
             (size_t)push->dword_count * sizeof(*push->dwords),
             push->relocations,
             (size_t)push->relocation_count * sizeof(*push->relocations));
   valid = NvxWinsysValidateBatch(push->device, batch, (uint32_t)total_bytes);
   free(batch);
   return valid;
}

int
NvxNouveauPushbufSubmitNvc0Clear(NVX_NOUVEAU_PUSHBUF *push,
                                 NVX_NOUVEAU_BO *target,
                                 uint64_t *submitted_fence)
{
   NVX_3D_NVC0_CLEAR request, result;
   NVX_3D_COLOR_SURFACE_LAYOUT layout;
   NVX_NOUVEAU_RELOCATION *reloc;
   uint64_t gpu_va;

   if (submitted_fence)
      *submitted_fence = 0;
   if (!submitted_fence || !push || !target || !push->device || !target->device ||
       !(push->device->caps.Features & NVX_3D_FEATURE_SUBMIT) ||
       push->device != target->device || !target->references ||
       !target->allocation.ClientToken ||
       push->client_token != target->allocation.ClientToken ||
       push->dword_count != NVX_3D_NVC0_CLEAR_DWORDS ||
       push->relocation_count != 2 ||
       !(target->allocation.Usage & NVX_3D_USAGE_COLOR_TARGET) ||
       (target->allocation.Usage & NVX_3D_USAGE_DEPTH_TARGET) ||
       !Nvx3dComputeColorSurfaceLayout(target->allocation.Width,
                                       target->allocation.Height, &layout) ||
       layout.PitchBytes != target->allocation.PitchBytes ||
       layout.TileMode != target->allocation.TileMode ||
       target->allocation.Bytes < layout.Bytes ||
       !NvxNouveauPushbufValidate(push))
      return 0;

   reloc = push->relocations;
   if (!reloc || reloc[0].CommandWord != 16 ||
       reloc[1].CommandWord != 17 ||
       reloc[0].BufferId != target->allocation.BufferId ||
       reloc[1].BufferId != target->allocation.BufferId ||
       reloc[0].BufferOffset != 0 || reloc[1].BufferOffset != 0 ||
       reloc[0].Bytes != target->allocation.Bytes ||
       reloc[1].Bytes != target->allocation.Bytes ||
       reloc[0].Access != (NVX_NOUVEAU_BO_READ | NVX_NOUVEAU_BO_WRITE) ||
       reloc[1].Access != (NVX_NOUVEAU_BO_READ | NVX_NOUVEAU_BO_WRITE) ||
       !NvxNouveauBoGetAddress(target, 0, target->allocation.Bytes,
                               NVX_NOUVEAU_BO_READ | NVX_NOUVEAU_BO_WRITE,
                               &gpu_va))
      return 0;

   memset(&request, 0, sizeof(request));
   request.Size = sizeof(request);
   request.Version = NVX_3D_ABI_VERSION;
   request.DwordCount = push->dword_count;
   request.BufferId = target->allocation.BufferId;
   memcpy(request.Dwords, push->dwords, sizeof(request.Dwords));
   /* The two address words are resolved from the live owned BO. The kernel
    * validates the exact clear profile and builds its own trusted queue data. */
   request.Dwords[16] = (uint32_t)(gpu_va >> 32);
   request.Dwords[17] = (uint32_t)gpu_va;
   memset(&result, 0, sizeof(result));
   if (!push->device->escape(push->device->opaque,
                             NVX_ESCAPE_3D_NVC0_NATIVE_CLEAR,
                             sizeof(request), &request,
                             sizeof(result), &result) ||
       result.Size != sizeof(result) ||
       result.Version != NVX_3D_ABI_VERSION ||
       result.DwordCount != NVX_3D_NVC0_CLEAR_DWORDS ||
       result.BufferId != target->allocation.BufferId ||
       !result.FenceValue ||
       !NvxNouveauBoMarkUsed(target, result.FenceValue))
      return 0;

   *submitted_fence = result.FenceValue;
   return 1;
}
