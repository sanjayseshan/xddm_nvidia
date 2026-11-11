#ifndef NVX_NOUVEAU_PUSHBUF_H
#define NVX_NOUVEAU_PUSHBUF_H

#include "nvx_nouveau_bo.h"

typedef struct NVX_NOUVEAU_PUSHBUF {
   NVX_WINSYS_DEVICE *device;
   uint64_t client_token;
   uint32_t *dwords;
   uint32_t dword_count;
   uint32_t dword_capacity;
   NVX_3D_BUFFER_REFERENCE *references;
   uint32_t reference_count;
   uint32_t reference_capacity;
   NVX_NOUVEAU_RELOCATION *relocations;
   uint32_t relocation_count;
   uint32_t relocation_capacity;
} NVX_NOUVEAU_PUSHBUF;

int NvxNouveauPushbufInit(NVX_NOUVEAU_PUSHBUF *push,
                          NVX_WINSYS_DEVICE *device, uint64_t client_token);
void NvxNouveauPushbufReset(NVX_NOUVEAU_PUSHBUF *push);
void NvxNouveauPushbufDestroy(NVX_NOUVEAU_PUSHBUF *push);
int NvxNouveauPushbufAppend(NVX_NOUVEAU_PUSHBUF *push,
                            const uint32_t *dwords, uint32_t count);
int NvxNouveauPushbufReference(NVX_NOUVEAU_PUSHBUF *push,
                               const NVX_NOUVEAU_BO *bo, uint32_t access);
int NvxNouveauPushbufRelocate(NVX_NOUVEAU_PUSHBUF *push,
                              const NVX_NOUVEAU_BO *bo,
                              uint32_t command_word, uint64_t offset,
                              uint64_t bytes, uint32_t access);
/* Structural kernel preflight only; not a GPU submission operation. */
int NvxNouveauPushbufValidate(NVX_NOUVEAU_PUSHBUF *push);
/* Submit the one native profile the kernel currently executes: a full-target
 * NVC0 color clear. This is deliberately not a general pushbuffer kick. */
int NvxNouveauPushbufSubmitNvc0Clear(NVX_NOUVEAU_PUSHBUF *push,
                                     NVX_NOUVEAU_BO *target,
                                     uint64_t *submitted_fence);

#endif
