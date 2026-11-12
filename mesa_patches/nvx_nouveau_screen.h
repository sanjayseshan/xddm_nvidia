#ifndef NVX_NOUVEAU_SCREEN_H
#define NVX_NOUVEAU_SCREEN_H

#include "nvx_nouveau_pushbuf.h"
#include "nvx_nouveau_device.h"

/* Minimal per-WGL-screen Nouveau state. Gallium resource and shader objects
 * are not implemented yet; this owns the validated NVX device and batch
 * builder used by the currently supported submission profile. */
typedef struct NVX_NOUVEAU_SCREEN {
   NVX_NOUVEAU_DEVICE *device;
   NVX_NOUVEAU_PUSHBUF pushbuf;
   uint64_t client_token;
   uint32_t graphics_class;
   uint32_t initialized;
} NVX_NOUVEAU_SCREEN;

int NvxNouveauScreenInit(NVX_NOUVEAU_SCREEN *screen,
                         NVX_NOUVEAU_DEVICE *device,
                         uint64_t client_token);
/* OS resource interface used by the XP winsys: allocations are session-owned,
 * mappings are staged and fence-synchronized by NVX_NOUVEAU_BO. */
int NvxNouveauScreenAllocBuffer(NVX_NOUVEAU_SCREEN *screen,
                                const NVX_3D_ALLOC_BUFFER *description,
                                NVX_NOUVEAU_BO *buffer);
void *NvxNouveauScreenMapBuffer(NVX_NOUVEAU_SCREEN *screen,
                                NVX_NOUVEAU_BO *buffer, uint32_t access);
int NvxNouveauScreenUnmapBuffer(NVX_NOUVEAU_SCREEN *screen,
                                NVX_NOUVEAU_BO *buffer);
int NvxNouveauScreenFreeBuffer(NVX_NOUVEAU_SCREEN *screen,
                                NVX_NOUVEAU_BO *buffer);
void NvxNouveauScreenDestroy(NVX_NOUVEAU_SCREEN *screen);
int NvxNouveauScreenSubmitNvc0Clear(NVX_NOUVEAU_SCREEN *screen,
                                    NVX_NOUVEAU_BO *target,
                                    const uint32_t *dwords,
                                    uint32_t dword_count,
                                    uint64_t *submitted_fence);

#endif
