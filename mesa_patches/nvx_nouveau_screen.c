#include "nvx_nouveau_screen.h"

#include <string.h>

int
NvxNouveauScreenInit(NVX_NOUVEAU_SCREEN *screen,
                     NVX_NOUVEAU_DEVICE *device,
                     uint64_t client_token)
{
   if (!screen || !device || !client_token ||
       device->class_3d != 0xc797U ||
       !(device->winsys.caps.Features & NVX_3D_FEATURE_SUBMIT))
      return 0;

   memset(screen, 0, sizeof(*screen));
   if (!NvxNouveauPushbufInit(&screen->pushbuf, &device->winsys,
                              client_token))
      return 0;

   screen->device = device;
   screen->client_token = client_token;
   screen->graphics_class = device->class_3d;
   screen->initialized = 1;
   return 1;
}

void
NvxNouveauScreenDestroy(NVX_NOUVEAU_SCREEN *screen)
{
   if (!screen)
      return;
   NvxNouveauPushbufDestroy(&screen->pushbuf);
   memset(screen, 0, sizeof(*screen));
}

int
NvxNouveauScreenSubmitNvc0Clear(NVX_NOUVEAU_SCREEN *screen,
                                NVX_NOUVEAU_BO *target,
                                const uint32_t *dwords,
                                uint32_t dword_count,
                                uint64_t *submitted_fence)
{
   if (submitted_fence)
      *submitted_fence = 0;
   if (!screen || !screen->initialized || !screen->device || !target || !dwords ||
       dword_count != NVX_3D_NVC0_CLEAR_DWORDS || !submitted_fence ||
       target->device != &screen->device->winsys || !target->references ||
       !target->allocation.ClientToken)
      return 0;

   NvxNouveauPushbufReset(&screen->pushbuf);
   screen->pushbuf.client_token = target->allocation.ClientToken;
   if (!NvxNouveauPushbufAppend(&screen->pushbuf, dwords, dword_count) ||
       !NvxNouveauPushbufRelocate(&screen->pushbuf, target, 16, 0,
                                  target->allocation.Bytes,
                                  NVX_NOUVEAU_BO_READ | NVX_NOUVEAU_BO_WRITE) ||
       !NvxNouveauPushbufRelocate(&screen->pushbuf, target, 17, 0,
                                  target->allocation.Bytes,
                                  NVX_NOUVEAU_BO_READ | NVX_NOUVEAU_BO_WRITE) ||
       !NvxNouveauPushbufSubmitNvc0Clear(&screen->pushbuf, target,
                                         submitted_fence)) {
      NvxNouveauPushbufReset(&screen->pushbuf);
      return 0;
   }
   NvxNouveauPushbufReset(&screen->pushbuf);
   return 1;
}
