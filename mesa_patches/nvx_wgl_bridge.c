/*
 * XP-specific bridge for the pinned Mesa WGL/softpipe build.
 *
 * This deliberately accepts only post-VS, constant-color triangles that fit
 * the miniport's bounded 64x16 kernel-owned draw operation. It is an opt-in
 * bring-up path, not a general Gallium driver or shader translator.
 */
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "tgsi/tgsi_info.h"
#include "draw/draw_vs.h"
#include "draw/draw_vertex.h"
#include "util/u_pack_color.h"
#include "softpipe/sp_context.h"
#include "softpipe/sp_state.h"
#include "softpipe/sp_tile_cache.h"
#include "../src_driver/nvx_3d_abi.h"
#include "nvx_nouveau_device.h"
#include "nvx_nouveau_screen.h"
#include "nvx_winsys_win32.h"
#include "xf86drm.h"

static HDC nvx_wgl_hdc;
static NVX_WINSYS_DEVICE nvx_bridge_winsys;
static NVX_NOUVEAU_DEVICE nvx_bridge_nouveau_device;
static NVX_NOUVEAU_SCREEN nvx_bridge_nouveau_screen;
static volatile LONG nvx_bridge_winsys_ready;
static volatile LONG nvx_bridge_drm_fd;
static volatile LONG nvx_bridge_nouveau_device_ready;
static volatile LONG nvx_bridge_nvx_adapter_ready;
static volatile LONG nvx_bridge_nouveau_device_id;
static volatile LONG nvx_bridge_nouveau_chipset;
static volatile LONG nvx_bridge_nouveau_aperture_low;
static volatile LONG nvx_bridge_nouveau_aperture_high;
static volatile LONG nvx_bridge_wgl_lock;
static HDC nvx_bridge_bound_hdc;
static volatile LONG nvx_bridge_trace_count;
static volatile LONG nvx_bridge_nouveau_init_failed;
static volatile LONG nvx_bridge_screen_count;
static volatile LONG nvx_bridge_screen_active;
static volatile LONG nvx_bridge_render_target_seen;
static struct pipe_screen *nvx_bridge_cached_nouveau_screen;
static volatile LONG nvx_bridge_callback_count;
static volatile LONG nvx_bridge_gpu_batch_count;
static volatile LONG nvx_bridge_nvc0_clear_count;
static volatile LONG nvx_bridge_last_primitive;
static volatile LONG nvx_bridge_last_count;
static volatile LONG nvx_bridge_last_triangle_count;
static volatile LONG nvx_bridge_last_clear_ms;
static volatile LONG nvx_bridge_last_draw_ms;
static volatile LONG nvx_bridge_last_readback_ms;
static volatile LONG nvx_bridge_last_upload_bits[8];
static char nvx_bridge_last_message[96];
static LONG nvx_bridge_depth_stage_count;
static char nvx_bridge_depth_stages[3][96];
static void nvx_bridge_trace(const char *message);
static void nvx_bridge_nouveau_trace(const char *message);
void NvxWglRecordMesaInitFailure(const char *stage, int error);
void NvxWglRecordMesaInitStage(const char *stage);
void NvxWglRecordMesaRenderTarget(uint32_t cbuf_count, uint32_t index,
                                  uint32_t present, uint32_t address_low,
                                  uint32_t address_high, uint32_t width,
                                  uint32_t height, uint32_t format,
                                  uint32_t memtype);
void NvxWglRecordNouveauSclass(uint32_t token_low, uint32_t token_high,
                               uint32_t result, uint32_t count,
                               const uint32_t *classes);
void NvxWglRecordNouveauPushbuf(uint32_t channel, uint32_t nr_push,
                                uint32_t nr_buffers, uint32_t nr_relocs);
void NvxWglRecordNouveauPushbufItem(uint32_t kind, uint32_t index,
                                    uint32_t a, uint32_t b, uint32_t c,
                                    uint32_t d, uint32_t e, uint32_t f);
void NvxWglRecordNouveauPushbufDwords(uint32_t push_index,
                                      uint32_t first_word,
                                      const uint32_t *dwords,
                                      uint32_t count);
void NvxWglRecordNouveauBufferDwords(uint32_t buffer_id, uint64_t gpu_va,
                                     uint32_t offset, const uint32_t *dwords,
                                     uint32_t count, uint32_t result);
void NvxWglRecordNouveauShaderDwords(uint32_t stage, uint32_t buffer_id,
                                     uint64_t gpu_va, uint32_t offset,
                                     const uint32_t *dwords, uint32_t count,
                                     uint32_t result, uint32_t draw_kind);
void NvxWglRecordNouveauObject(uint32_t token_low, uint32_t token_high,
                               uint32_t user_class, uint32_t user_handle,
                               uint32_t rm_class, uint32_t rm_handle,
                               uint32_t result);
void NvxWglRecordNouveauIoctl(uint32_t type, uint32_t version,
                              uint32_t route, uint32_t method,
                              uint32_t size);
void NvxWglRecordNouveauGetparam(uint32_t param, uint32_t value,
                                 uint32_t result);
void NvxWglRecordNouveauDrawState(const uint32_t *methods,
                                 const uint32_t *values, uint32_t count,
                                 uint32_t draw_kind);
static int nvx_bridge_is_enabled(void);
static int nvx_pack_color(const float rgba[4], uint32_t *color);
static int nvx_alloc(HDC hdc, const NVX_3D_ALLOC_BUFFER *request,
                     NVX_3D_ALLOC_BUFFER *result);
static int nvx_bridge_release_targets(HDC hdc);
void nvx_wgl_notify_clear(struct softpipe_context *sp, unsigned buffers,
                          const union pipe_color_union *color, double depth);

#define NVX_BRIDGE_TARGET_SLOTS 8
struct nvx_bridge_target_state {
   struct softpipe_context *sp;
   NVX_NOUVEAU_BO color_bo;
   unsigned width, height;
   NVX_3D_COLOR_SURFACE_LAYOUT color_layout;
   NVX_3D_DEPTH_SURFACE_LAYOUT depth_layout;
   uint64_t color_id, depth_id, vbo_id;
   uint64_t color_gpu_va;
   uint64_t color_client_token;
   uint32_t color_bytes;
   uint32_t clear_color;
   unsigned clear_color_pending : 1;
   unsigned clear_depth_pending : 1;
};
static struct nvx_bridge_target_state nvx_bridge_targets[NVX_BRIDGE_TARGET_SLOTS];

static struct nvx_bridge_target_state *
nvx_bridge_target_state(struct softpipe_context *sp, int create)
{
   unsigned i, free_slot = NVX_BRIDGE_TARGET_SLOTS;
   for (i = 0; i < NVX_BRIDGE_TARGET_SLOTS; i++) {
      if (nvx_bridge_targets[i].sp == sp)
         return &nvx_bridge_targets[i];
      if (!nvx_bridge_targets[i].sp && free_slot == NVX_BRIDGE_TARGET_SLOTS)
         free_slot = i;
   }
   if (!create || free_slot == NVX_BRIDGE_TARGET_SLOTS)
      return NULL;
   memset(&nvx_bridge_targets[free_slot], 0,
          sizeof(nvx_bridge_targets[free_slot]));
   nvx_bridge_targets[free_slot].sp = sp;
   nvx_bridge_targets[free_slot].clear_color = 0xff000000U;
   nvx_bridge_targets[free_slot].clear_color_pending = 1;
   nvx_bridge_targets[free_slot].clear_depth_pending = 1;
   return &nvx_bridge_targets[free_slot];
}

void
nvx_wgl_notify_clear(struct softpipe_context *sp, unsigned buffers,
                     const union pipe_color_union *color, double depth)
{
   struct nvx_bridge_target_state *state;
   uint32_t packed;
   (void)depth;
   if (!sp || !nvx_bridge_is_enabled())
      return;
   state = nvx_bridge_target_state(sp, 1);
   if (!state)
      return;
   if ((buffers & PIPE_CLEAR_COLOR) && color &&
       nvx_pack_color(color->f, &packed)) {
      state->clear_color = packed;
      state->clear_color_pending = 1;
   }
   if (buffers & PIPE_CLEAR_DEPTH)
      state->clear_depth_pending = 1;
}

struct nvx_bridge_status {
   uint32_t size;
   uint32_t version;
   LONG screen_count;
   LONG callback_count;
   LONG gpu_batch_count;
   LONG nvc0_clear_count;
   LONG last_primitive;
   LONG last_count;
   LONG last_triangle_count;
   LONG last_clear_ms;
   LONG last_draw_ms;
   LONG last_readback_ms;
   LONG last_upload_bits[8];
   char last_message[96];
   LONG depth_stage_count;
   char depth_stages[3][96];
   LONG nouveau_device_ready;
   LONG nouveau_device_id;
   LONG nouveau_chipset;
   LONG nouveau_aperture_low;
   LONG nouveau_aperture_high;
   NVX_XF86DRM_STATUS xf86drm;
};

__declspec(dllexport) BOOL WINAPI
NvxQueryBridgeStatus(struct nvx_bridge_status *status, DWORD bytes);

__declspec(dllexport) BOOL WINAPI
NvxQueryBridgeStatus(struct nvx_bridge_status *status, DWORD bytes)
{
   if (!status || bytes < sizeof(*status))
      return FALSE;
   status->size = sizeof(*status);
   status->version = 5;
   status->screen_count = InterlockedCompareExchange(&nvx_bridge_screen_count, 0, 0);
   status->callback_count = InterlockedCompareExchange(&nvx_bridge_callback_count, 0, 0);
   status->gpu_batch_count = InterlockedCompareExchange(&nvx_bridge_gpu_batch_count, 0, 0);
   status->nvc0_clear_count = InterlockedCompareExchange(
      &nvx_bridge_nvc0_clear_count, 0, 0);
   status->last_primitive = InterlockedCompareExchange(&nvx_bridge_last_primitive, 0, 0);
   status->last_count = InterlockedCompareExchange(&nvx_bridge_last_count, 0, 0);
   status->last_triangle_count = InterlockedCompareExchange(&nvx_bridge_last_triangle_count, 0, 0);
   status->last_clear_ms = InterlockedCompareExchange(&nvx_bridge_last_clear_ms, 0, 0);
   status->last_draw_ms = InterlockedCompareExchange(&nvx_bridge_last_draw_ms, 0, 0);
   status->last_readback_ms = InterlockedCompareExchange(&nvx_bridge_last_readback_ms, 0, 0);
   {
      unsigned i;
      for (i = 0; i < 8; i++)
         status->last_upload_bits[i] = InterlockedCompareExchange(
            &nvx_bridge_last_upload_bits[i], 0, 0);
   }
   memcpy(status->last_message, nvx_bridge_last_message,
          sizeof(status->last_message));
   status->depth_stage_count = InterlockedCompareExchange(
      &nvx_bridge_depth_stage_count, 0, 0);
   memcpy(status->depth_stages, nvx_bridge_depth_stages,
          sizeof(status->depth_stages));
   status->nouveau_device_ready = InterlockedCompareExchange(
      &nvx_bridge_nouveau_device_ready, 0, 0);
   status->nouveau_device_id = InterlockedCompareExchange(
      &nvx_bridge_nouveau_device_id, 0, 0);
   status->nouveau_chipset = InterlockedCompareExchange(
      &nvx_bridge_nouveau_chipset, 0, 0);
   status->nouveau_aperture_low = InterlockedCompareExchange(
      &nvx_bridge_nouveau_aperture_low, 0, 0);
   status->nouveau_aperture_high = InterlockedCompareExchange(
      &nvx_bridge_nouveau_aperture_high, 0, 0);
   NvxXf86DrmGetStatus(&status->xf86drm);
   return TRUE;
}

void nvx_wgl_set_hdc(HDC hdc);
int nvx_wgl_xf86drm_fd(void);
int nvx_wgl_nouveau_device_available(void);
void nvx_wgl_notify_hybrid_screen_created(void);
int nvx_wgl_nvx_adapter_create(void);
struct pipe_screen *nvx_wgl_create_native_nouveau_screen(void);
struct pipe_screen *nouveau_drm_screen_create(int drm_fd);
int nvx_wgl_try_softpipe_triangle(struct softpipe_context *sp,
                                  const void *vertices,
                                  const uint16_t *indices, unsigned stride,
                                  unsigned primitive, unsigned count);

void
nvx_wgl_set_hdc(HDC hdc)
{
   LONG ready;
   LONG old_fd;

   if (!hdc)
      return;
   while (InterlockedCompareExchange(&nvx_bridge_wgl_lock, 1, 0) != 0)
      Sleep(0);

   /* DrvSetContext calls this again for every context switch. Reopening the
    * NVX client on the same DC leaked one miniport client session per switch. */
   if (hdc == nvx_bridge_bound_hdc &&
       InterlockedCompareExchange(&nvx_bridge_winsys_ready, 0, 0)) {
      InterlockedExchange(&nvx_bridge_wgl_lock, 0);
      return;
   }

   /* Mesa owns one WGL pipe_screen for the ICD process. Keep its adapter
    * transport alive after the first native screen is created; rebinding the
    * fd would invalidate BOs and channel state still owned by that screen. */
   if (nvx_bridge_bound_hdc && hdc != nvx_bridge_bound_hdc &&
       InterlockedCompareExchange(&nvx_bridge_screen_count, 0, 0) != 0) {
      InterlockedExchange(&nvx_bridge_wgl_lock, 0);
      return;
   }

   InterlockedExchange(&nvx_bridge_screen_active, 0);
   old_fd = InterlockedCompareExchange(&nvx_bridge_drm_fd, 0, 0);
   if (old_fd > 0) {
      if (!nvx_bridge_release_targets(nvx_bridge_bound_hdc)) {
         nvx_bridge_trace("NVX bridge HDC switch deferred: a render target is still busy");
         InterlockedExchange(&nvx_bridge_wgl_lock, 0);
         return;
      }
      InterlockedExchange(&nvx_bridge_drm_fd, 0);
      nvx_bridge_cached_nouveau_screen = NULL;
      if (InterlockedCompareExchange(&nvx_bridge_nvx_adapter_ready, 0, 0)) {
         NvxNouveauScreenDestroy(&nvx_bridge_nouveau_screen);
         InterlockedExchange(&nvx_bridge_nvx_adapter_ready, 0);
      }
      NvxXf86DrmUnregisterDevice(old_fd);
      /* Unregister closes the session through the device copy it owns. */
      nvx_bridge_winsys.ClientToken = 0;
   } else if (nvx_bridge_winsys.ClientToken) {
      if (!nvx_bridge_release_targets(nvx_bridge_bound_hdc)) {
         nvx_bridge_trace("NVX bridge HDC switch deferred: a render target is still busy");
         InterlockedExchange(&nvx_bridge_wgl_lock, 0);
         return;
      }
      (void)NvxWinsysCloseClient(&nvx_bridge_winsys);
   }
   nvx_bridge_bound_hdc = hdc;
   InterlockedExchangePointer((PVOID volatile *)&nvx_wgl_hdc, hdc);
   ready = NvxWinsysInitHdc(&nvx_bridge_winsys, hdc);
   if (ready) {
      int fd = NvxXf86DrmRegisterDevice(&nvx_bridge_winsys);
      if (fd > 0)
         InterlockedExchange(&nvx_bridge_drm_fd, fd);
      else
         ready = 0;
   }
   InterlockedExchange(&nvx_bridge_winsys_ready, ready);
   ready = ready && NvxNouveauDeviceInitFromWinsys(
      &nvx_bridge_nouveau_device, &nvx_bridge_winsys);
   InterlockedExchange(&nvx_bridge_nouveau_device_ready, ready);
   if (ready) {
      InterlockedExchange(&nvx_bridge_nouveau_device_id,
                          (LONG)nvx_bridge_nouveau_device.device_id);
      InterlockedExchange(&nvx_bridge_nouveau_chipset,
                          (LONG)nvx_bridge_nouveau_device.chipset);
      InterlockedExchange(&nvx_bridge_nouveau_aperture_low,
                          (LONG)(uint32_t)nvx_bridge_nouveau_device.vram_aperture_bytes);
      InterlockedExchange(&nvx_bridge_nouveau_aperture_high,
                          (LONG)(uint32_t)(nvx_bridge_nouveau_device.vram_aperture_bytes >> 32));
   } else {
      InterlockedExchange(&nvx_bridge_nouveau_device_id, 0);
      InterlockedExchange(&nvx_bridge_nouveau_chipset, 0);
      InterlockedExchange(&nvx_bridge_nouveau_aperture_low, 0);
      InterlockedExchange(&nvx_bridge_nouveau_aperture_high, 0);
   }
   nvx_bridge_trace("NVX bridge device probe completed");
   InterlockedExchange(&nvx_bridge_wgl_lock, 0);
}

int
nvx_wgl_xf86drm_fd(void)
{
   return InterlockedCompareExchange(&nvx_bridge_drm_fd, 0, 0);
}

int
nvx_wgl_nouveau_device_available(void)
{
   return InterlockedCompareExchange(&nvx_bridge_nouveau_device_ready, 0, 0) != 0;
}

void
nvx_wgl_notify_hybrid_screen_created(void)
{
   InterlockedExchange(&nvx_bridge_screen_active, 1);
   InterlockedIncrement(&nvx_bridge_screen_count);
   nvx_bridge_trace("NVX hybrid WGL screen created; Gallium backend is softpipe");
}

int
nvx_wgl_nvx_adapter_create(void)
{
   LONG ready;

   if (!nvx_wgl_nouveau_device_available())
      return 0;
   ready = InterlockedCompareExchange(&nvx_bridge_nvx_adapter_ready, 0, 0);
   if (!ready) {
      uint64_t token = nvx_bridge_winsys.ClientToken;
      ready = NvxNouveauScreenInit(&nvx_bridge_nouveau_screen,
                                   &nvx_bridge_nouveau_device, token);
      InterlockedExchange(&nvx_bridge_nvx_adapter_ready, ready);
   }
   if (!ready)
      return 0;

   nvx_bridge_trace("NVX adapter ready (bounded NVC0 clear; Gallium Nouveau unavailable)");
   return 1;
}

struct pipe_screen *
nvx_wgl_create_native_nouveau_screen(void)
{
   int fd = nvx_wgl_xf86drm_fd();
   struct pipe_screen *screen;

   if (nvx_bridge_cached_nouveau_screen) {
      nvx_bridge_nouveau_trace("Returning cached native Nouveau Gallium pipe_screen");
      return nvx_bridge_cached_nouveau_screen;
   }

   InterlockedExchange(&nvx_bridge_nouveau_init_failed, 0);
   nvx_bridge_nouveau_trace("Nouveau screen create begin");
   if (fd <= 0 || !NvxXf86DrmIsRegistered(fd))
      return NULL;
   NvxNouveauBoPoolEnable(1);
   screen = nouveau_drm_screen_create(fd);
   if (screen) {
      nvx_bridge_nouveau_trace("Native Nouveau Gallium pipe_screen created");
      nvx_bridge_cached_nouveau_screen = screen;
      InterlockedIncrement(&nvx_bridge_screen_count);
   } else {
      if (!InterlockedCompareExchange(&nvx_bridge_nouveau_init_failed, 0, 0))
         nvx_bridge_nouveau_trace("Native Nouveau screen creation failed; see DRM boundary");
   }
   return screen;
}

static int
nvx_find_output(const struct draw_vertex_shader *vs, unsigned semantic)
{
   unsigned i;

   if (!vs)
      return -1;
   for (i = 0; i < vs->info.num_outputs; i++) {
      if (vs->info.output_semantic_name[i] == semantic &&
          vs->info.output_semantic_index[i] == 0)
         return (int)i;
   }
   return -1;
}

static int
nvx_float01(float value)
{
   return value == value && value >= 0.0f && value <= 1.0f;
}

static void
nvx_bridge_trace(const char *message)
{
   HANDLE file;
   DWORD written;
   size_t length = strlen(message);
   LONG count = InterlockedIncrement(&nvx_bridge_trace_count);

   if (length >= sizeof(nvx_bridge_last_message))
      length = sizeof(nvx_bridge_last_message) - 1;
   memcpy(nvx_bridge_last_message, message, length);
   nvx_bridge_last_message[length] = '\0';

   if (count > 32 && strncmp(message, "NVC0 init rc=", 13) != 0 &&
       strncmp(message, "NVIF SCLASS", 11) != 0 &&
       strncmp(message, "Nouveau pushbuf", 15) != 0 &&
       strncmp(message, "Nouveau screen", 14) != 0 &&
       strncmp(message, "Native Nouveau", 14) != 0)
      return;
   file = CreateFileA("C:\\test_accel\\mesa_bridge.log", FILE_APPEND_DATA,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, NULL);
   if (file == INVALID_HANDLE_VALUE)
      return;
   WriteFile(file, message, (DWORD)strlen(message), &written, NULL);
   WriteFile(file, "\r\n", 2, &written, NULL);
   CloseHandle(file);
}

/* Called by the external NVC0 patch to identify screen-init failure stages. */
void
NvxWglRecordMesaInitFailure(const char *stage, int error)
{
   char message[96];
   char prefix[32];
   size_t prefix_length, stage_length, copy_length;

   if (!stage)
      stage = "unknown stage";
   wsprintfA(prefix, "NVC0 init rc=%d: ", error);
   prefix_length = strlen(prefix);
   if (prefix_length >= sizeof(message))
      prefix_length = sizeof(message) - 1U;
   memcpy(message, prefix, prefix_length);
   stage_length = strlen(stage);
   copy_length = sizeof(message) - prefix_length - 1U;
   if (stage_length > copy_length)
      stage_length = copy_length;
   memcpy(message + prefix_length, stage, stage_length);
   message[prefix_length + stage_length] = '\0';
   InterlockedExchange(&nvx_bridge_nouveau_init_failed, 1);
   nvx_bridge_nouveau_trace(message);
}

void
NvxWglRecordNouveauSclass(uint32_t token_low, uint32_t token_high,
                          uint32_t result, uint32_t count,
                          const uint32_t *classes)
{
   char message[96];
   unsigned i;

   wsprintfA(message, "NVIF SCLASS token=%08lx:%08lx result=%lu count=%lu",
             (unsigned long)token_high, (unsigned long)token_low,
             (unsigned long)result, (unsigned long)count);
   nvx_bridge_nouveau_trace(message);
   if (!classes)
      return;
   for (i = 0; i < count && i < 4U; i++) {
      wsprintfA(message, "NVIF SCLASS class[%lu]=%08lx",
                (unsigned long)i, (unsigned long)classes[i]);
      nvx_bridge_nouveau_trace(message);
   }
}

void
NvxWglRecordNouveauPushbuf(uint32_t channel, uint32_t nr_push,
                           uint32_t nr_buffers, uint32_t nr_relocs)
{
   char message[96];

   InterlockedIncrement(&nvx_bridge_gpu_batch_count);
   wsprintfA(message, "Nouveau pushbuf channel=%08lx pushes=%lu buffers=%lu relocs=%lu",
             (unsigned long)channel, (unsigned long)nr_push,
             (unsigned long)nr_buffers, (unsigned long)nr_relocs);
   nvx_bridge_nouveau_trace(message);
}

void
NvxWglRecordNouveauPushbufItem(uint32_t kind, uint32_t index,
                               uint32_t a, uint32_t b, uint32_t c,
                               uint32_t d, uint32_t e, uint32_t f)
{
   char message[112];

   wsprintfA(message,
             "Nouveau pushbuf item kind=%lu index=%lu a=%08lx b=%08lx c=%08lx d=%08lx e=%08lx f=%08lx",
             (unsigned long)kind, (unsigned long)index,
             (unsigned long)a, (unsigned long)b, (unsigned long)c,
             (unsigned long)d, (unsigned long)e, (unsigned long)f);
   nvx_bridge_nouveau_trace(message);
}

void
NvxWglRecordNouveauPushbufDwords(uint32_t push_index, uint32_t first_word,
                                 const uint32_t *dwords, uint32_t count)
{
   static volatile LONG captured_pushes;
   char output[48U * 1024U];
   char line[128];
   uint32_t i, used = 0;
   LONG capture_index;
   HANDLE file;
   DWORD written;

   if (!dwords || !count || count > 4096U ||
       !InterlockedCompareExchange(&nvx_bridge_render_target_seen, 0, 0))
      return;
   capture_index = InterlockedIncrement(&captured_pushes);
   /* Capture streams only after Mesa has bound a real framebuffer, and keep
    * the sample bounded for offline packet decoding. */
   if (capture_index > 32)
      return;
   for (i = 0; i < count; i += 8U) {
      uint32_t j, end = count - i;
      int length;
      if (end > 8U)
         end = 8U;
      length = wsprintfA(line, "Nouveau dwords push=%lu capture=%lu offset=%lu:",
                         (unsigned long)push_index,
                         (unsigned long)capture_index,
                         (unsigned long)(first_word + i));
      for (j = 0; j < end && length < (int)sizeof(line) - 10; j++)
         length += wsprintfA(line + length, " %08lx",
                             (unsigned long)dwords[i + j]);
      if (length + 2 >= (int)sizeof(output) - (int)used)
         break;
      memcpy(output + used, line, (size_t)length);
      used += (uint32_t)length;
      output[used++] = '\r';
      output[used++] = '\n';
   }
   if (!used)
      return;
   file = CreateFileA("C:\\test_accel\\nvx_nouveau_trace.log",
                      FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   if (file == INVALID_HANDLE_VALUE)
      return;
   WriteFile(file, output, used, &written, NULL);
   CloseHandle(file);
}

void
NvxWglRecordNouveauDrawState(const uint32_t *methods, const uint32_t *values,
                             uint32_t count, uint32_t draw_kind)
{
   char output[8192];
   char line[96];
   uint32_t i, used = 0;
   HANDLE file;
   DWORD written;

   if (!methods || !values || !count || count > 80U)
      return;
   {
      const char *label = draw_kind == 2U ? "GLSL_TRIANGLES/3" :
                          draw_kind == 3U ? "FFVS_GLSL_FS_TRIANGLES/3" :
                          draw_kind == 4U ? "GLSL_IMMEDIATE_TRIANGLES/3" :
                          draw_kind == 5U ? "FULL_TARGET_DRAW" :
                                            "QUADS/24";
      int length = wsprintfA(line,
         "Nouveau draw state trace_rev=130 draw=%s entries=%lu\r\n",
         label, (unsigned long)count);
      if (length > 0 && (uint32_t)length <= sizeof(output) - used) {
         memcpy(output + used, line, (size_t)length);
         used += (uint32_t)length;
      }
   }
   for (i = 0; i < count; i++) {
      int length = wsprintfA(line,
         "Nouveau draw state draw=%s index=%lu method_dec=%lu value=%08lx\r\n",
         draw_kind == 2U ? "GLSL_TRIANGLES/3" :
         draw_kind == 3U ? "FFVS_GLSL_FS_TRIANGLES/3" :
         draw_kind == 4U ? "GLSL_IMMEDIATE_TRIANGLES/3" :
         draw_kind == 5U ? "FULL_TARGET_DRAW" : "QUADS/24",
         (unsigned long)i, (unsigned long)methods[i],
         (unsigned long)values[i]);
      if (length <= 0 || (uint32_t)length > sizeof(output) - used)
         break;
      memcpy(output + used, line, (size_t)length);
      used += (uint32_t)length;
   }
   if (!used)
      return;
   file = CreateFileA("C:\\test_accel\\nvx_nouveau_trace.log",
                      FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   if (file == INVALID_HANDLE_VALUE)
      return;
   WriteFile(file, output, used, &written, NULL);
   CloseHandle(file);
}

void
NvxWglRecordNouveauBufferDwords(uint32_t buffer_id, uint64_t gpu_va,
                                uint32_t offset, const uint32_t *dwords,
                                uint32_t count, uint32_t result)
{
   char output[48U * 192U];
   char line[192];
   uint32_t i, used = 0;
   HANDLE file;
   DWORD written;

   if (!dwords || !count || count > 256U)
      return;
   _snprintf(line, sizeof(line) - 1U,
      "Nouveau vertex dump buffer=%08lx gpu=%08lx:%08lx offset=%lu dwords=%lu source=%lu\r\n",
      (unsigned long)buffer_id, (unsigned long)(gpu_va >> 32),
      (unsigned long)gpu_va, (unsigned long)offset,
      (unsigned long)count, (unsigned long)result);
   line[sizeof(line) - 1U] = '\0';
   used = (uint32_t)strlen(line);
   if (used >= sizeof(output))
      return;
   memcpy(output, line, used);
   for (i = 0; i < count && used < sizeof(output); i += 6U) {
      uint32_t j, end = count - i;
      int length;
      if (end > 6U)
         end = 6U;
      length = _snprintf(line, sizeof(line) - 1U,
                         "Nouveau vertex data offset=%lu:",
                         (unsigned long)(offset + i * sizeof(uint32_t)));
      if (length < 0)
         break;
      for (j = 0; j < end && length < (int)sizeof(line) - 10; j++)
         length += _snprintf(line + length, sizeof(line) - (size_t)length - 1U,
                             " %08lx", (unsigned long)dwords[i + j]);
      if (length <= 0 || (uint32_t)length + 2U > sizeof(output) - used)
         break;
      memcpy(output + used, line, (size_t)length);
      used += (uint32_t)length;
      output[used++] = '\r';
      output[used++] = '\n';
   }
   file = CreateFileA("C:\\test_accel\\nvx_nouveau_trace.log",
                      FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   if (file == INVALID_HANDLE_VALUE)
      return;
   WriteFile(file, output, used, &written, NULL);
   CloseHandle(file);
}

void
NvxWglRecordNouveauShaderDwords(uint32_t stage, uint32_t buffer_id,
                                uint64_t gpu_va, uint32_t offset,
                                const uint32_t *dwords, uint32_t count,
                                uint32_t result, uint32_t draw_kind)
{
   char output[24U * 192U];
   char line[192];
   uint32_t i, used = 0;
   HANDLE file;
   DWORD written;

   if (!dwords || !count || count > 128U)
      return;
   _snprintf(line, sizeof(line) - 1U,
      "Nouveau shader dump draw=%s stage=%lu buffer=%08lx gpu=%08lx:%08lx offset=%lu dwords=%lu source=%lu draw_snapshot=%lu\r\n",
      draw_kind == 2U ? "GLSL_TRIANGLES/3" :
      draw_kind == 3U ? "FFVS_GLSL_FS_TRIANGLES/3" :
      draw_kind == 5U ? "FULL_TARGET_DRAW" : "QUADS/24",
      (unsigned long)stage, (unsigned long)buffer_id,
      (unsigned long)(gpu_va >> 32), (unsigned long)gpu_va,
      (unsigned long)offset, (unsigned long)count,
      (unsigned long)(result == 4U ? 0U : result),
      (unsigned long)(result == 4U));
   line[sizeof(line) - 1U] = '\0';
   used = (uint32_t)strlen(line);
   if (used >= sizeof(output))
      return;
   memcpy(output, line, used);
   for (i = 0; i < count && used < sizeof(output); i += 6U) {
      uint32_t j, end = count - i;
      int length;
      if (end > 6U)
         end = 6U;
      length = _snprintf(line, sizeof(line) - 1U,
                         "Nouveau shader data offset=%lu:",
                         (unsigned long)(offset + i * sizeof(uint32_t)));
      if (length < 0)
         break;
      for (j = 0; j < end && length < (int)sizeof(line) - 10; j++)
         length += _snprintf(line + length, sizeof(line) - (size_t)length - 1U,
                             " %08lx", (unsigned long)dwords[i + j]);
      if (length <= 0 || (uint32_t)length + 2U > sizeof(output) - used)
         break;
      memcpy(output + used, line, (size_t)length);
      used += (uint32_t)length;
      output[used++] = '\r';
      output[used++] = '\n';
   }
   file = CreateFileA("C:\\test_accel\\nvx_nouveau_trace.log",
                      FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   if (file == INVALID_HANDLE_VALUE)
      return;
   WriteFile(file, output, used, &written, NULL);
   CloseHandle(file);
}

void
NvxWglRecordNouveauObject(uint32_t token_low, uint32_t token_high,
                          uint32_t user_class, uint32_t user_handle,
                          uint32_t rm_class, uint32_t rm_handle,
                          uint32_t result)
{
   char message[96];

   wsprintfA(message,
             "NVIF NEW token=%08lx:%08lx class=%08lx handle=%08lx rm=%08lx/%08lx result=%lu",
             (unsigned long)token_high, (unsigned long)token_low,
             (unsigned long)user_class, (unsigned long)user_handle,
             (unsigned long)rm_class, (unsigned long)rm_handle,
             (unsigned long)result);
   nvx_bridge_nouveau_trace(message);
}

void
NvxWglRecordNouveauIoctl(uint32_t type, uint32_t version,
                         uint32_t route, uint32_t method, uint32_t size)
{
   char message[96];

   wsprintfA(message,
             "NVIF WRITE_READ type=%08lx version=%lu route=%08lx method=%08lx size=%lu",
             (unsigned long)type, (unsigned long)version,
             (unsigned long)route, (unsigned long)method,
             (unsigned long)size);
   nvx_bridge_nouveau_trace(message);
}

void
NvxWglRecordNouveauGetparam(uint32_t param, uint32_t value, uint32_t result)
{
   char message[80];

   wsprintfA(message, "DRM GETPARAM param=%lu value=%08lx result=%lu",
             (unsigned long)param, (unsigned long)value,
             (unsigned long)result);
   nvx_bridge_nouveau_trace(message);
}

void
NvxWglRecordMesaInitStage(const char *stage)
{
   if (stage)
      nvx_bridge_nouveau_trace(stage);
}

void
NvxWglRecordMesaRenderTarget(uint32_t cbuf_count, uint32_t index,
                             uint32_t present, uint32_t address_low,
                             uint32_t address_high, uint32_t width,
                             uint32_t height, uint32_t format,
                             uint32_t memtype)
{
   char message[176];

   if (present)
      InterlockedExchange(&nvx_bridge_render_target_seen, 1);
   wsprintfA(message,
      "NVC0 framebuffer cbufs=%lu index=%lu present=%lu address=%08lx:%08lx size=%lux%lu format=%08lx memtype=%08lx",
      (unsigned long)cbuf_count, (unsigned long)index,
      (unsigned long)present, (unsigned long)address_high,
      (unsigned long)address_low, (unsigned long)width,
      (unsigned long)height, (unsigned long)format,
      (unsigned long)memtype);
   nvx_bridge_nouveau_trace(message);
}

static void
nvx_bridge_nouveau_trace(const char *message)
{
   HANDLE file;
   DWORD written;

   file = CreateFileA("C:\\test_accel\\nvx_nouveau_trace.log",
                      FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   if (file == INVALID_HANDLE_VALUE)
      return;
   WriteFile(file, message, (DWORD)strlen(message), &written, NULL);
   WriteFile(file, "\r\n", 2, &written, NULL);
   CloseHandle(file);
}

/* Use the bounded NVX bridge by default on the NVX WGL screen. An explicit
 * environment value still lets tests render software references or force a
 * diagnostic pass. */
static int
nvx_bridge_is_enabled(void)
{
   char enabled[8];
   if (GetEnvironmentVariableA("NVX_MESA_GALLIUM_BRIDGE", enabled,
                               sizeof(enabled)))
      return enabled[0] == '1';
   return InterlockedCompareExchange(&nvx_bridge_screen_active, 0, 0) != 0;
}

static int
nvx_pack_color(const float rgba[4], uint32_t *color)
{
   unsigned r, g, b, a;
   unsigned i;

   for (i = 0; i < 4; i++)
      if (!nvx_float01(rgba[i]))
         return 0;
   r = (unsigned)(rgba[0] * 255.0f + 0.5f);
   g = (unsigned)(rgba[1] * 255.0f + 0.5f);
   b = (unsigned)(rgba[2] * 255.0f + 0.5f);
   a = (unsigned)(rgba[3] * 255.0f + 0.5f);
   *color = (a << 24) | (r << 16) | (g << 8) | b;
   return 1;
}

static int
nvx_upload(HDC hdc, uint64_t buffer_id, const float *vertices,
           unsigned vertex_bytes)
{
   if (vertex_bytes >= 32) {
      unsigned i, count = vertex_bytes / sizeof(float);
      if (count > 8) count = 8;
      for (i = 0; i < count; i++) {
         LONG bits;
         memcpy(&bits, &vertices[i], sizeof(bits));
         InterlockedExchange(&nvx_bridge_last_upload_bits[i], bits);
      }
   }
   if (!hdc || !buffer_id || vertex_bytes < 72 || vertex_bytes > 4096 ||
       (vertex_bytes != 96 && vertex_bytes % 24))
      return 0;
   return InterlockedCompareExchange(&nvx_bridge_winsys_ready, 0, 0) &&
      NvxWinsysUploadBuffer(&nvx_bridge_winsys, buffer_id, 4096, 0,
                            vertices, vertex_bytes);
}

static int
nvx_alloc(HDC hdc, const NVX_3D_ALLOC_BUFFER *request,
          NVX_3D_ALLOC_BUFFER *result)
{
   return hdc && InterlockedCompareExchange(&nvx_bridge_winsys_ready, 0, 0) &&
      NvxWinsysAllocBuffer(&nvx_bridge_winsys, request, result);
}

static void
nvx_free(HDC hdc, uint64_t buffer_id)
{
   if (hdc && buffer_id &&
       InterlockedCompareExchange(&nvx_bridge_winsys_ready, 0, 0))
      NvxWinsysFreeBuffer(&nvx_bridge_winsys, buffer_id);
}

static int
nvx_bridge_release_targets(HDC hdc)
{
   unsigned i;

   for (i = 0; i < NVX_BRIDGE_TARGET_SLOTS; i++) {
      struct nvx_bridge_target_state *target = &nvx_bridge_targets[i];

      if (target->color_bo.references) {
         if (!NvxNouveauBoDestroy(&target->color_bo))
            return 0;
      } else if (target->color_id) {
         nvx_free(hdc, target->color_id);
      }
      if (target->depth_id)
         nvx_free(hdc, target->depth_id);
      if (target->vbo_id)
         nvx_free(hdc, target->vbo_id);
      memset(target, 0, sizeof(*target));
   }
   return 1;
}

static int
nvx_wait_fence(HDC hdc, uint64_t fence_value)
{
   return hdc && InterlockedCompareExchange(&nvx_bridge_winsys_ready, 0, 0) &&
      NvxWinsysWaitFence(&nvx_bridge_winsys, fence_value, 10000);
}

static int
nvx_clear_target(HDC hdc, uint64_t target_id, uint32_t color)
{
   NVX_3D_SUBMIT submit, submitted;
   memset(&submit, 0, sizeof(submit));
   submit.Size = sizeof(submit);
   submit.Version = NVX_3D_ABI_VERSION;
   submit.ClientToken = nvx_bridge_winsys.ClientToken;
   submit.RequestToken = NvxWinsysNextRequestToken(&nvx_bridge_winsys);
   submit.Operation = NVX_3D_SUBMIT_CLEAR_SURFACE;
   submit.Width = NVX_3D_TRIANGLE_WIDTH;
   submit.Height = NVX_3D_TRIANGLE_HEIGHT;
   submit.Color = color;
   submit.BufferId = target_id;
   memset(&submitted, 0, sizeof(submitted));
   return ExtEscape(hdc, NVX_ESCAPE_3D_SUBMIT, sizeof(submit),
                    (LPCSTR)&submit, sizeof(submitted),
                    (LPSTR)&submitted) != 0 &&
          submitted.FenceValue && nvx_wait_fence(hdc, submitted.FenceValue);
}

static int
nvx_submit_triangle(HDC hdc, uint64_t target_id, uint64_t vbo_id,
                    const float *vertices, uint32_t color,
                    int depth_test)
{
   NVX_3D_SUBMIT submit, submitted;
   if (!nvx_upload(hdc, vbo_id, vertices, depth_test ? 96 : 72))
      return 0;

   memset(&submit, 0, sizeof(submit));
   submit.Size = sizeof(submit);
   submit.Version = NVX_3D_ABI_VERSION;
   submit.ClientToken = nvx_bridge_winsys.ClientToken;
   submit.RequestToken = NvxWinsysNextRequestToken(&nvx_bridge_winsys);
   submit.Operation = depth_test ? NVX_3D_SUBMIT_DRAW_TRIANGLE_DEPTH :
                                   NVX_3D_SUBMIT_DRAW_TRIANGLE_PRESERVE;
   submit.Width = NVX_3D_TRIANGLE_WIDTH;
   submit.Height = NVX_3D_TRIANGLE_HEIGHT;
   submit.Color = color;
   submit.BufferId = target_id;
   submit.Reserved[0] = (uint32_t)vbo_id;
   submit.Reserved[1] = (uint32_t)(vbo_id >> 32);
   memset(&submitted, 0, sizeof(submitted));
   if (!ExtEscape(hdc, NVX_ESCAPE_3D_SUBMIT, sizeof(submit),
                  (LPCSTR)&submit, sizeof(submitted),
                  (LPSTR)&submitted) || !submitted.FenceValue)
      return 0;
   return nvx_wait_fence(hdc, submitted.FenceValue);
}

static int
nvx_submit_triangles(HDC hdc, uint64_t target_id, uint64_t vbo_id,
                     const float *vertices, unsigned vertex_count,
                     uint32_t color)
{
   NVX_3D_SUBMIT submit, submitted;
   unsigned bytes = vertex_count * 6 * sizeof(float);
   if (vertex_count < 3 || vertex_count > NVX_3D_MAX_BATCH_VERTICES ||
       vertex_count % 3 || bytes > 4096) {
      nvx_bridge_trace("batch reject: invalid vertex count or byte size");
      return 0;
   }
   if (!nvx_upload(hdc, vbo_id, vertices, bytes)) {
      nvx_bridge_trace("batch upload failed");
      return 0;
   }

   memset(&submit, 0, sizeof(submit));
   memset(&submitted, 0, sizeof(submitted));
   submit.Size = sizeof(submit);
   submit.Version = NVX_3D_ABI_VERSION;
   submit.ClientToken = nvx_bridge_winsys.ClientToken;
   submit.RequestToken = NvxWinsysNextRequestToken(&nvx_bridge_winsys);
   submit.Operation = NVX_3D_SUBMIT_DRAW_TRIANGLES;
   submit.Width = NVX_3D_TRIANGLE_WIDTH;
   submit.Height = vertex_count;
   submit.Color = color;
   submit.BufferId = target_id;
   submit.Reserved[0] = (uint32_t)vbo_id;
   submit.Reserved[1] = (uint32_t)(vbo_id >> 32);
   if (!ExtEscape(hdc, NVX_ESCAPE_3D_SUBMIT, sizeof(submit),
                  (LPCSTR)&submit, sizeof(submitted),
                  (LPSTR)&submitted) || !submitted.FenceValue) {
      nvx_bridge_trace("batch submit failed or returned no fence");
      return 0;
   }
   if (!nvx_wait_fence(hdc, submitted.FenceValue)) {
      nvx_bridge_trace("batch fence wait failed");
      return 0;
   }
   return 1;
}

struct nvx_bridge_vertex {
   float x, y, z;
};

struct nvx_bridge_triangle {
   struct nvx_bridge_vertex v[3];
   uint32_t color;
};

static unsigned
nvx_clip_triangle_to_tile(const struct nvx_bridge_triangle *triangle,
                          float left, float top, float right, float bottom,
                          struct nvx_bridge_vertex output[12]);
static int
nvx_update_softpipe_tile(struct softpipe_context *sp,
                         const uint32_t pixels[NVX_3D_READBACK_PIXELS],
                         int x0, int y0, int flip_y);

static int
nvx_submit_dynamic_target_batch(HDC hdc, const NVX_3D_COLOR_SURFACE_LAYOUT *layout,
                                uint64_t target_id, uint64_t vbo_id,
                                uint64_t depth_target_id,
                                unsigned depth_mode,
                                const float *vertices, unsigned vertex_count,
                                uint32_t color)
{
   NVX_3D_SUBMIT submit, submitted;
   unsigned stride = depth_target_id ? 8U : 6U;
   unsigned bytes = vertex_count * stride * sizeof(float);

   if (!vertex_count || vertex_count > NVX_3D_MAX_BATCH_VERTICES ||
       vertex_count % 3 || !nvx_upload(hdc, vbo_id, vertices, bytes))
      return 0;

   memset(&submit, 0, sizeof(submit));
   submit.Size = sizeof(submit);
   submit.Version = NVX_3D_ABI_VERSION;
   submit.ClientToken = nvx_bridge_winsys.ClientToken;
   submit.RequestToken = NvxWinsysNextRequestToken(&nvx_bridge_winsys);
   submit.Operation = !depth_target_id ? NVX_3D_SUBMIT_DRAW_COLOR_TARGET :
      depth_mode==2U ? NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH_ATTACH :
      depth_mode==3U ? NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH_TEST_ONLY :
                       NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH;
   submit.Width = layout->Width;
   submit.Height = layout->Height;
   submit.Color = color;
   submit.BufferId = target_id;
   submit.Reserved[0] = vertex_count;
   submit.Reserved[1] = (uint32_t)vbo_id;
   submit.Reserved[2] = (uint32_t)(vbo_id >> 32);
   submit.Reserved[3] = layout->TileMode;
   submit.Reserved[4] = (uint32_t)depth_target_id;
   submit.Reserved[5] = (uint32_t)(depth_target_id >> 32);
   memset(&submitted, 0, sizeof(submitted));
   return ExtEscape(hdc, NVX_ESCAPE_3D_SUBMIT, sizeof(submit),
                    (LPCSTR)&submit, sizeof(submitted),
                    (LPSTR)&submitted) != 0 && submitted.FenceValue &&
          nvx_wait_fence(hdc, submitted.FenceValue);
}

static int
nvx_clear_dynamic_depth_target(HDC hdc,
                               const NVX_3D_DEPTH_SURFACE_LAYOUT *layout,
                               uint64_t target_id)
{
   NVX_3D_SUBMIT submit, submitted;
   memset(&submit, 0, sizeof(submit));
   submit.Size = sizeof(submit);
   submit.Version = NVX_3D_ABI_VERSION;
   submit.ClientToken = nvx_bridge_winsys.ClientToken;
   submit.RequestToken = NvxWinsysNextRequestToken(&nvx_bridge_winsys);
   submit.Operation = NVX_3D_SUBMIT_CLEAR_COLOR_TARGET_DEPTH;
   submit.Width = layout->Width;
   submit.Height = layout->Height;
   submit.BufferId = target_id;
   memset(&submitted, 0, sizeof(submitted));
   return ExtEscape(hdc, NVX_ESCAPE_3D_SUBMIT, sizeof(submit),
                    (LPCSTR)&submit, sizeof(submitted),
                    (LPSTR)&submitted) != 0 && submitted.FenceValue &&
          nvx_wait_fence(hdc, submitted.FenceValue);
}

/* Feed the softpipe render-target clear through the narrow NVC0 packet ABI.
 * The miniport validates the packet against the owned allocation and builds
 * its trusted C797 operation. The packet includes a bounded full-target clear
 * rectangle so results do not depend on stale state from a previous GR job. */
static int
nvx_clear_dynamic_target_nvc0(HDC hdc,
                              const NVX_3D_COLOR_SURFACE_LAYOUT *layout,
                              NVX_NOUVEAU_BO *target, uint32_t color)
{
   NVX_3D_COLOR_SURFACE_LAYOUT expected_layout;
   uint32_t dwords[NVX_3D_NVC0_CLEAR_DWORDS] = {0};
   float rgba[4];
   uint64_t submitted_fence = 0;
   unsigned i;

   (void)hdc;
   if (!layout || !target || !target->references ||
       !InterlockedCompareExchange(&nvx_bridge_nvx_adapter_ready, 0, 0) ||
       !Nvx3dComputeColorSurfaceLayout(layout->Width, layout->Height,
                                      &expected_layout) ||
       expected_layout.PitchBytes != layout->PitchBytes ||
       expected_layout.TileMode != layout->TileMode ||
       expected_layout.Bytes > target->allocation.Bytes ||
       target->allocation.Width != layout->Width ||
       target->allocation.Height != layout->Height ||
       target->allocation.PitchBytes != layout->PitchBytes ||
       target->allocation.TileMode != layout->TileMode)
      return 0;

   rgba[0] = ((color >> 16) & 0xffU) / 255.0f;
   rgba[1] = ((color >> 8) & 0xffU) / 255.0f;
   rgba[2] = (color & 0xffU) / 255.0f;
   rgba[3] = ((color >> 24) & 0xffU) / 255.0f;
   dwords[0] = Nvx3dNvc0SeqHeader(0x0d80U, 4U);
   for (i = 0; i < 4; i++)
      memcpy(&dwords[1 + i], &rgba[i], sizeof(rgba[i]));
   dwords[5] = Nvx3dNvc0SeqHeader(0x0ff4U, 2U);
   dwords[6] = layout->Width << 16;
   dwords[7] = layout->Height << 16;
   dwords[8] = Nvx3dNvc0SeqHeader(0x0d6cU, 2U);
   dwords[9] = layout->Width << 16;
   dwords[10] = layout->Height << 16;
   dwords[11] = Nvx3dNvc0SeqHeader(0x10f8U, 1U);
   dwords[12] = 0x10U;
   dwords[13] = Nvx3dNvc0SeqHeader(0x121cU, 1U);
   dwords[14] = 1U;
   dwords[15] = Nvx3dNvc0SeqHeader(0x0800U, 9U);
   dwords[16] = (uint32_t)(target->allocation.GpuVa >> 32);
   dwords[17] = (uint32_t)target->allocation.GpuVa;
   dwords[18] = layout->Width;
   dwords[19] = layout->Height;
   dwords[20] = 0xcfU;
   dwords[21] = layout->TileMode;
   dwords[22] = 1U;
   dwords[23] = target->allocation.Bytes / 4U;
   dwords[25] = Nvx3dNvc0ImmediateHeader(0x15d0U, 0U);
   dwords[26] = Nvx3dNvc0NonIncHeader(0x19d0U, 1U);
   dwords[27] = 0x3cU;

   if (!NvxNouveauScreenSubmitNvc0Clear(&nvx_bridge_nouveau_screen,
                                        target, dwords,
                                        NVX_3D_NVC0_CLEAR_DWORDS,
                                        &submitted_fence) ||
       !NvxWinsysWaitFence(&nvx_bridge_winsys, submitted_fence, 10000))
      return 0;
   InterlockedIncrement(&nvx_bridge_nvc0_clear_count);
   return 1;
}

static int
nvx_read_dynamic_target_tile(HDC hdc, uint64_t target_id, unsigned x, unsigned y,
                             uint32_t pixels[NVX_3D_READBACK_PIXELS])
{
   NVX_3D_READBACK request;
   memset(&request, 0, sizeof(request));
   request.Size = sizeof(request);
   request.Version = NVX_3D_ABI_VERSION;
   request.BufferId = target_id;
   request.Width = NVX_3D_TRIANGLE_WIDTH;
   request.Height = NVX_3D_TRIANGLE_HEIGHT;
   request.Reserved[0] = x;
   request.Reserved[1] = y;
   return ExtEscape(hdc, NVX_ESCAPE_3D_READBACK, sizeof(request),
                    (LPCSTR)&request, NVX_3D_READBACK_BYTES,
                    (LPSTR)pixels) != 0;
}

/* One full-size target avoids the old per-64x16-tile C797 submissions. The
 * supported bridge state remains intentionally narrow; other state falls
 * back to software. */
static int
nvx_bridge_full_target(struct softpipe_context *sp, HDC hdc,
                       const struct nvx_bridge_triangle *triangles,
                       unsigned triangle_count, uint32_t background,
                       unsigned viewport_width, unsigned viewport_height,
                       int depth_test, unsigned depth_mode)
{
   NVX_3D_COLOR_SURFACE_LAYOUT layout;
   NVX_3D_ALLOC_BUFFER target, target_result, vbo, vbo_result;
   NVX_3D_DEPTH_SURFACE_LAYOUT depth_layout = {0};
   NVX_3D_ALLOC_BUFFER depth_target, depth_result;
   struct nvx_bridge_target_state *target_state;
   float batches[12][NVX_3D_MAX_BATCH_VERTICES * 8];
   float batch_vertices_data[12][NVX_3D_MAX_BATCH_VERTICES * 8];
   uint32_t batch_colors[12];
   unsigned batch_vertices[12], batch_count = 0;
   struct nvx_bridge_vertex clipped[12];
   uint32_t pixels[NVX_3D_READBACK_PIXELS];
   uint32_t center_sample = 0, far_sample = 0;
   uint32_t far_neighbors[4] = {0, 0, 0, 0};
   unsigned far_tile_x = 0, far_tile_y = 0;
   unsigned read_start_x, read_start_y, read_end_x, read_end_y;
   unsigned center_x = viewport_width / 2, center_y = viewport_height / 2;
   unsigned far_x = viewport_width / 6, far_y = viewport_height / 6;
   float min_x, min_y, max_x, max_y;
   unsigned triangle, i, x, y;
   int ok = 0;
   DWORD stage_start;
   const char *stage = "target validation";

   memset(&target, 0, sizeof(target));
   memset(&target_result, 0, sizeof(target_result));
   memset(&vbo, 0, sizeof(vbo));
   memset(&vbo_result, 0, sizeof(vbo_result));
   memset(&depth_target, 0, sizeof(depth_target));
   memset(&depth_result, 0, sizeof(depth_result));
   if (!sp || !hdc || !triangle_count || triangle_count > 12 ||
       sp->framebuffer.width != viewport_width ||
       sp->framebuffer.height != viewport_height ||
       !Nvx3dComputeColorSurfaceLayout(viewport_width, viewport_height,
                                       &layout) ||
       (depth_test &&
        !Nvx3dComputeDepthSurfaceLayout(viewport_width, viewport_height,
                                        &depth_layout))) {
      nvx_bridge_trace("full target reject: requires full-frame viewport/layout");
      return 0;
   }
   target_state = nvx_bridge_target_state(sp, 1);
   if (!target_state) {
      nvx_bridge_trace("full target reject: target-state table full");
      return 0;
   }
   if (target_state->width != viewport_width ||
       target_state->height != viewport_height) {
      if (target_state->color_bo.references &&
          !NvxNouveauBoDestroy(&target_state->color_bo)) {
         nvx_bridge_trace("full target resize blocked: color BO still in use");
         return 0;
      }
      if (target_state->depth_id)
         nvx_free(hdc, target_state->depth_id);
      if (target_state->vbo_id)
         nvx_free(hdc, target_state->vbo_id);
      target_state->color_id = target_state->depth_id =
         target_state->vbo_id = 0;
      target_state->color_gpu_va = 0;
      target_state->color_client_token = 0;
      target_state->color_bytes = 0;
      target_state->width = viewport_width;
      target_state->height = viewport_height;
      target_state->color_layout = layout;
      target_state->depth_layout = depth_layout;
      target_state->clear_color_pending = 1;
      target_state->clear_depth_pending = 1;
   }
   min_x = max_x = triangles[0].v[0].x;
   min_y = max_y = triangles[0].v[0].y;
   for (triangle = 0; triangle < triangle_count; triangle++) {
      for (i = 0; i < 3; i++) {
         if (triangles[triangle].v[i].x < min_x) min_x = triangles[triangle].v[i].x;
         if (triangles[triangle].v[i].x > max_x) max_x = triangles[triangle].v[i].x;
         if (triangles[triangle].v[i].y < min_y) min_y = triangles[triangle].v[i].y;
         if (triangles[triangle].v[i].y > max_y) max_y = triangles[triangle].v[i].y;
      }
   }

   target.Size = sizeof(target);
   target.Version = NVX_3D_ABI_VERSION;
   target.Usage = NVX_3D_USAGE_COLOR_TARGET;
   target.Bytes = layout.Bytes;
   target.Width = layout.Width;
   target.Height = layout.Height;
   target.PitchBytes = layout.PitchBytes;
   target.TileMode = layout.TileMode;
   target.ClientToken = nvx_bridge_winsys.ClientToken;
   if (!target_state->color_id) {
      stage = "color allocation";
      if (!NvxNouveauBoCreate(&nvx_bridge_nouveau_device.winsys, &target,
                              target.ClientToken,
                              &target_state->color_bo))
         goto done;
      target_result = target_state->color_bo.allocation;
      target_state->color_id = target_result.BufferId;
      target_state->color_gpu_va = target_result.GpuVa;
      target_state->color_client_token = target_result.ClientToken;
      target_state->color_bytes = target_result.Bytes;
      target_state->clear_color_pending = 1;
   } else {
      target_result.BufferId = target_state->color_id;
      target_result.GpuVa = target_state->color_gpu_va;
      target_result.Bytes = target_state->color_bytes;
      target_result.ClientToken = target_state->color_client_token;
   }

   vbo.Size = sizeof(vbo);
   vbo.Version = NVX_3D_ABI_VERSION;
   vbo.Usage = 1;
   vbo.Bytes = 4096;
   if (!target_state->vbo_id) {
      vbo.ClientToken = nvx_bridge_winsys.ClientToken;
      stage = "vertex allocation";
      if (!nvx_alloc(hdc, &vbo, &vbo_result))
         goto done;
      target_state->vbo_id = vbo_result.BufferId;
   } else {
      vbo_result.BufferId = target_state->vbo_id;
   }

   if (depth_test && !target_state->depth_id) {
      stage = "depth allocation";
      depth_target.Size = sizeof(depth_target);
      depth_target.Version = NVX_3D_ABI_VERSION;
      depth_target.Usage = NVX_3D_USAGE_DEPTH_TARGET |
                           NVX_3D_USAGE_DEPTH_ZF32;
      depth_target.Bytes = NVX_3D_DEPTH_TARGET_OFFSET + depth_layout.Bytes;
      depth_target.Width = depth_layout.Width;
      depth_target.Height = depth_layout.Height;
      depth_target.PitchBytes = depth_layout.PitchBytes;
      depth_target.ClientToken = nvx_bridge_winsys.ClientToken;
      if (!nvx_alloc(hdc, &depth_target, &depth_result))
         goto done;
      target_state->depth_id = depth_result.BufferId;
      target_state->depth_layout = depth_layout;
      target_state->clear_depth_pending = 1;
   } else if (target_state->depth_id) {
      depth_result.BufferId = target_state->depth_id;
   }
   if (depth_test && target_state->clear_depth_pending) {
      stage = "depth clear";
      stage_start = GetTickCount();
      if (!nvx_clear_dynamic_depth_target(hdc, &depth_layout,
                                          target_state->depth_id)) goto done;
      InterlockedExchange(&nvx_bridge_last_clear_ms,
                          (LONG)(GetTickCount() - stage_start));
      target_state->clear_depth_pending = 0;
   }
   if (target_state->clear_color_pending) {
      stage = "color clear";
      stage_start = GetTickCount();
      if (!nvx_clear_dynamic_target_nvc0(
             hdc, &layout, &target_state->color_bo,
             target_state->clear_color))
         goto done;
      InterlockedExchange(&nvx_bridge_last_clear_ms,
                          (LONG)(GetTickCount() - stage_start));
      target_state->clear_color_pending = 0;
   }
   memset(batch_vertices, 0, sizeof(batch_vertices));

   for (triangle = 0; triangle < triangle_count; triangle++) {
      unsigned polygon_count = nvx_clip_triangle_to_tile(
         &triangles[triangle], 0.0f, 0.0f, (float)viewport_width,
         (float)viewport_height, clipped);
      unsigned fan;
      for (fan = 1; fan + 1 < polygon_count; fan++) {
         struct nvx_bridge_vertex fan_vertices[3] = {
            clipped[0], clipped[fan], clipped[fan + 1]
         };
         float hw_vertices[24];
         unsigned vertex_stride = depth_test ? 8U : 6U;
         unsigned batch, j;
         /* With depth enabled, preserve the original primitive submission
          * order. Grouping faces by color changes depth-write order and makes
          * the result depend on the order of the six cube faces. */
         if (depth_test) {
            /* Consecutive primitives of one face can share a submission:
             * vertex order is retained, and the draw color is unchanged. */
            if (batch_count &&
                batch_colors[batch_count - 1] == triangles[triangle].color &&
                batch_vertices[batch_count - 1] + 3 <=
                   NVX_3D_MAX_BATCH_VERTICES) {
               batch = batch_count - 1;
            } else {
               batch = batch_count++;
               if (batch >= 12)
                  goto done;
               batch_colors[batch] = triangles[triangle].color;
            }
         } else {
            for (batch = 0; batch < batch_count; batch++)
               if (batch_colors[batch] == triangles[triangle].color)
                  break;
         }
         if (!depth_test && batch == batch_count) {
            if (batch_count >= 12)
               goto done;
            batch_colors[batch] = triangles[triangle].color;
            batch_count++;
         }
         if (batch_vertices[batch] + 3 > NVX_3D_MAX_BATCH_VERTICES) {
            stage = "depth/color draw batch";
            if (!nvx_submit_dynamic_target_batch(hdc, &layout,
                  target_result.BufferId, vbo_result.BufferId,
                  depth_test ? target_state->depth_id : 0,
                  depth_mode,
                  batch_vertices_data[batch], batch_vertices[batch], batch_colors[batch]))
               goto done;
            batch_vertices[batch] = 0;
         }
         for (j = 0; j < 3; j++) {
            unsigned base = j * vertex_stride;
            float local_z = (fan_vertices[j].z + 1.0f) * 0.5f;
            if (!nvx_float01(local_z))
               goto done;
            hw_vertices[base + 0] =
               2.0f * fan_vertices[j].x / (float)viewport_width - 1.0f;
            hw_vertices[base + 1] =
               2.0f * fan_vertices[j].y / (float)viewport_height - 1.0f;
            hw_vertices[base + 2] = local_z;
            if (depth_test) {
               hw_vertices[base + 3] = 1.0f;
               hw_vertices[base + 4] = local_z;
               hw_vertices[base + 5] = 1.0f;
               hw_vertices[base + 6] = 0.0f;
               hw_vertices[base + 7] = 1.0f;
            } else {
               hw_vertices[base + 3] = 0.0f;
               hw_vertices[base + 4] = 0.0f;
               hw_vertices[base + 5] = 1.0f;
            }
         }
         memcpy(&batch_vertices_data[batch][batch_vertices[batch] * vertex_stride],
                hw_vertices, vertex_stride * 3U * sizeof(float));
         batch_vertices[batch] += 3;
      }
   }

   stage_start = GetTickCount();
   if (depth_test) {
      /* Flush each primitive in source order to preserve Mesa's depth writes. */
      for (i = 0; i < batch_count; i++) {
         stage = "depth/color draw batch";
         if (batch_vertices[i] &&
             !nvx_submit_dynamic_target_batch(hdc, &layout,
                  target_result.BufferId, vbo_result.BufferId,
                  target_state->depth_id, depth_mode, batch_vertices_data[i],
                  batch_vertices[i], batch_colors[i]))
            goto done;
      }
   } else for (i = 0; i < batch_count; i++) {
      stage = "depth/color draw batch";
      if (batch_vertices[i] &&
          !nvx_submit_dynamic_target_batch(hdc, &layout,
               target_result.BufferId, vbo_result.BufferId,
               depth_test ? target_state->depth_id : 0,
               depth_test ? depth_mode : 0, batch_vertices_data[i],
               batch_vertices[i], batch_colors[i]))
         goto done;
   }
   InterlockedExchange(&nvx_bridge_last_draw_ms,
                       (LONG)(GetTickCount() - stage_start));

   stage = "target readback";
   stage_start = GetTickCount();
   read_start_x = min_x <= 0.0f ? 0U : (unsigned)min_x;
   read_start_y = min_y <= 0.0f ? 0U : (unsigned)min_y;
   read_end_x = max_x >= (float)viewport_width ? viewport_width - 1U :
                (unsigned)max_x;
   read_end_y = max_y >= (float)viewport_height ? viewport_height - 1U :
                (unsigned)max_y;
   read_start_x &= ~(NVX_3D_TRIANGLE_WIDTH - 1U);
   read_start_y &= ~(NVX_3D_TRIANGLE_HEIGHT - 1U);
   read_end_x = (read_end_x & ~(NVX_3D_TRIANGLE_WIDTH - 1U)) +
                NVX_3D_TRIANGLE_WIDTH;
   read_end_y = (read_end_y & ~(NVX_3D_TRIANGLE_HEIGHT - 1U)) +
                NVX_3D_TRIANGLE_HEIGHT;
   if (read_end_x > viewport_width) read_end_x = viewport_width;
   if (read_end_y > viewport_height) read_end_y = viewport_height;
   for (y = read_start_y; y < read_end_y; y += NVX_3D_TRIANGLE_HEIGHT) {
      for (x = read_start_x; x < read_end_x; x += NVX_3D_TRIANGLE_WIDTH) {
         if (!nvx_read_dynamic_target_tile(hdc, target_result.BufferId,
                                           x, y, pixels) ||
             !nvx_update_softpipe_tile(sp, pixels, (int)x, (int)y, 1))
            goto done;
         if (center_x >= x && center_x < x + NVX_3D_TRIANGLE_WIDTH &&
             center_y >= y && center_y < y + NVX_3D_TRIANGLE_HEIGHT)
            center_sample = pixels[(center_y - y) * NVX_3D_TRIANGLE_WIDTH +
                                   (center_x - x)];
         if (far_x >= x && far_x < x + NVX_3D_TRIANGLE_WIDTH &&
             far_y >= y && far_y < y + NVX_3D_TRIANGLE_HEIGHT) {
            far_sample = pixels[(far_y - y) * NVX_3D_TRIANGLE_WIDTH +
                                (far_x - x)];
            if (far_x > x)
               far_neighbors[0] = pixels[(far_y - y) * NVX_3D_TRIANGLE_WIDTH +
                                         (far_x - x - 1)];
            if (far_x + 1 < viewport_width && far_x + 1 < x + 64)
               far_neighbors[1] = pixels[(far_y - y) * NVX_3D_TRIANGLE_WIDTH +
                                         (far_x - x + 1)];
            if (far_y > y)
               far_neighbors[2] = pixels[(far_y - y - 1) * NVX_3D_TRIANGLE_WIDTH +
                                         (far_x - x)];
            if (far_y + 1 < viewport_height && far_y + 1 < y + 16)
               far_neighbors[3] = pixels[(far_y - y + 1) * NVX_3D_TRIANGLE_WIDTH +
                                         (far_x - x)];
            far_tile_x = x;
            far_tile_y = y;
         }
      }
   }
   InterlockedExchange(&nvx_bridge_last_readback_ms,
                       (LONG)(GetTickCount() - stage_start));
   sp_flush_tile_cache(sp->cbuf_cache[0]);
   ok = 1;
done:
   if (ok) {
      char message[96];
      unsigned cache_far_y = viewport_height - 1 - far_y;
      struct softpipe_cached_tile *far_tile = sp_get_cached_tile(
         sp->cbuf_cache[0], (int)far_x, (int)cache_far_y, 0);
      unsigned cache_far = 0;
      if (far_tile) {
         float *rgba = far_tile->data.color[cache_far_y & (TILE_SIZE - 1)]
                                             [far_x & (TILE_SIZE - 1)];
         cache_far = ((unsigned)(rgba[3] * 255.0f + 0.5f) << 24) |
                     ((unsigned)(rgba[0] * 255.0f + 0.5f) << 16) |
                     ((unsigned)(rgba[1] * 255.0f + 0.5f) << 8) |
                     (unsigned)(rgba[2] * 255.0f + 0.5f);
      }
      wsprintfA(message,
         "full c=%08lx f=%08lx cache=%08lx",
         (unsigned long)center_sample, (unsigned long)far_sample,
         (unsigned long)cache_far);
      nvx_bridge_trace(message);
      wsprintfA(message, "full far xy=%u,%u tile=%u,%u local=%u,%u",
         far_x, far_y, far_tile_x, far_tile_y,
         far_x - far_tile_x, far_y - far_tile_y);
      nvx_bridge_trace(message);
      wsprintfA(message, "full far adj=%08lx,%08lx,%08lx,%08lx",
         (unsigned long)far_neighbors[0], (unsigned long)far_neighbors[1],
         (unsigned long)far_neighbors[2], (unsigned long)far_neighbors[3]);
      nvx_bridge_trace(message);
   } else {
      char message[96];
      wsprintfA(message, "full target failed at %s; software fallback retained",
                stage);
      nvx_bridge_trace(message);
   }
   return ok;
}

static int
nvx_clear_depth(HDC hdc, uint64_t target_id)
{
   NVX_3D_SUBMIT submit, submitted;
   memset(&submit, 0, sizeof(submit));
   submit.Size = sizeof(submit);
   submit.Version = NVX_3D_ABI_VERSION;
   submit.ClientToken = nvx_bridge_winsys.ClientToken;
   submit.RequestToken = NvxWinsysNextRequestToken(&nvx_bridge_winsys);
   submit.Operation = NVX_3D_SUBMIT_CLEAR_DEPTH;
   submit.Width = NVX_3D_TRIANGLE_WIDTH;
   submit.Height = NVX_3D_TRIANGLE_HEIGHT;
   submit.BufferId = target_id;
   memset(&submitted, 0, sizeof(submitted));
   return ExtEscape(hdc, NVX_ESCAPE_3D_SUBMIT, sizeof(submit),
                    (LPCSTR)&submit, sizeof(submitted),
                    (LPSTR)&submitted) != 0 && submitted.FenceValue &&
          nvx_wait_fence(hdc, submitted.FenceValue);
}

static int
nvx_readback_target(HDC hdc, uint64_t target_id,
                    uint32_t pixels[NVX_3D_READBACK_PIXELS])
{
   NVX_3D_READBACK readback;
   memset(&readback, 0, sizeof(readback));
   readback.Size = sizeof(readback);
   readback.Version = NVX_3D_ABI_VERSION;
   readback.BufferId = target_id;
   readback.Width = NVX_3D_TRIANGLE_WIDTH;
   readback.Height = NVX_3D_TRIANGLE_HEIGHT;
   return ExtEscape(hdc, NVX_ESCAPE_3D_READBACK, sizeof(readback),
                    (LPCSTR)&readback, NVX_3D_READBACK_BYTES,
                    (LPSTR)pixels) == NVX_3D_READBACK_BYTES;
}

static int
nvx_update_softpipe_tile(struct softpipe_context *sp,
                         const uint32_t pixels[NVX_3D_READBACK_PIXELS],
                         int x0, int y0, int flip_y)
{
   int x, y;

   if (!sp->framebuffer.nr_cbufs) {
      nvx_bridge_trace("reject framebuffer: no color buffers");
      return 0;
   }
   if (!sp->cbuf_cache[0]) {
      nvx_bridge_trace("reject framebuffer: color tile cache is null");
      return 0;
   }
   if (sp->framebuffer.cbufs[0].format != PIPE_FORMAT_B8G8R8A8_UNORM &&
       sp->framebuffer.cbufs[0].format != PIPE_FORMAT_B8G8R8X8_UNORM) {
      nvx_bridge_trace("reject framebuffer: unsupported pipe format");
      return 0;
   }
   if (x0 < 0 || y0 < 0 || x0 >= (int)sp->framebuffer.width ||
       y0 >= (int)sp->framebuffer.height) {
      nvx_bridge_trace("reject viewport: target outside framebuffer bounds");
      return 0;
   }
   if (x0 & 63) {
      nvx_bridge_trace("reject viewport: target x is not tile aligned");
      return 0;
   }
   for (y = 0; y < 16; y++) {
      int src_y = y0 + y;
      int dst_y = flip_y ? (int)sp->framebuffer.height - 1 - src_y : src_y;
      struct softpipe_cached_tile *tile;
      if (y0 + y >= (int)sp->framebuffer.height || dst_y < 0)
         continue;
      tile = sp_get_cached_tile(sp->cbuf_cache[0], x0, dst_y, 0);
      if (!tile) {
         nvx_bridge_trace("reject framebuffer: tile lookup returned null");
         return 0;
      }
      for (x = 0; x < 64; x++) {
         if (x0 + x >= (int)sp->framebuffer.width)
            continue;
         uint32_t pixel = pixels[y * 64 + x];
         float *rgba;
         rgba = tile->data.color[dst_y & (TILE_SIZE - 1)]
                                [(x0 + x) & (TILE_SIZE - 1)];
         rgba[0] = ((pixel >> 16) & 0xff) / 255.0f;
         rgba[1] = ((pixel >> 8) & 0xff) / 255.0f;
         rgba[2] = (pixel & 0xff) / 255.0f;
         rgba[3] = sp->framebuffer.cbufs[0].format ==
                   PIPE_FORMAT_B8G8R8X8_UNORM ? 1.0f :
                   ((pixel >> 24) & 0xff) / 255.0f;
      }
   }
   return 1;
}

static unsigned
nvx_clip_axis(const struct nvx_bridge_vertex *input, unsigned count,
              struct nvx_bridge_vertex *output, unsigned axis, float bound,
              int keep_greater)
{
   unsigned i, out_count = 0;
   struct nvx_bridge_vertex previous;
   int previous_inside;

   if (!count)
      return 0;
   previous = input[count - 1];
   previous_inside = keep_greater ?
      (axis ? previous.y : previous.x) >= bound :
      (axis ? previous.y : previous.x) <= bound;
   for (i = 0; i < count; i++) {
      struct nvx_bridge_vertex current = input[i];
      float p = axis ? previous.y : previous.x;
      float c = axis ? current.y : current.x;
      int current_inside = keep_greater ? c >= bound : c <= bound;
      if (current_inside != previous_inside && c != p) {
         float t = (bound - p) / (c - p);
         struct nvx_bridge_vertex intersection;
         intersection.x = previous.x + t * (current.x - previous.x);
         intersection.y = previous.y + t * (current.y - previous.y);
         intersection.z = previous.z + t * (current.z - previous.z);
         output[out_count++] = intersection;
      }
      if (current_inside)
         output[out_count++] = current;
      previous = current;
      previous_inside = current_inside;
   }
   return out_count;
}

static unsigned
nvx_clip_triangle_to_tile(const struct nvx_bridge_triangle *triangle,
                          float left, float top, float right, float bottom,
                          struct nvx_bridge_vertex output[12])
{
   struct nvx_bridge_vertex a[12], b[12];
   unsigned count = 3;

   memcpy(a, triangle->v, sizeof(triangle->v));
   count = nvx_clip_axis(a, count, b, 0, left, 1);
   count = nvx_clip_axis(b, count, a, 0, right, 0);
   count = nvx_clip_axis(a, count, b, 1, top, 1);
   count = nvx_clip_axis(b, count, a, 1, bottom, 0);
   memcpy(output, a, count * sizeof(*output));
   return count;
}

int
nvx_wgl_try_softpipe_triangle(struct softpipe_context *sp,
                              const void *vertices,
                              const uint16_t *indices, unsigned stride,
                              unsigned primitive, unsigned count)
{
   HDC hdc;
   int position_output, color_output;
   const struct vertex_info *vinfo;
   const float *v[3];
   struct nvx_bridge_triangle triangles[12];
   struct nvx_bridge_vertex clipped[12];
   float batches[12][NVX_3D_MAX_BATCH_VERTICES * 6];
   uint32_t batch_colors[12];
   unsigned batch_vertices[12];
   float hw_vertices[24];
   float rgba[4];
   uint32_t pixels[NVX_3D_READBACK_PIXELS];
   const struct pipe_viewport_state *viewport;
   NVX_3D_QUERY_CAPS caps;
   NVX_3D_ALLOC_BUFFER target, target_result, vbo, vbo_result;
   uint32_t color, background;
   unsigned i, triangle, triangle_count, stored = 0;
   unsigned viewport_width, viewport_height;
   float viewport_abs_x, viewport_abs_y;
   int viewport_x, viewport_y, tile_x, tile_y;
   int tile_start_x, tile_start_y, tile_end_x, tile_end_y;
   int is_quads, depth_test;
   unsigned depth_mode;
   int full_target = 0;
   int ok = 0;
   int caps_escape;
   const char *stage = "bridge validation";

   if (!nvx_bridge_is_enabled())
      return 0;
   InterlockedIncrement(&nvx_bridge_callback_count);
   if (!sp || !vertices) {
      nvx_bridge_trace("reject: missing softpipe context or vertices");
      return 0;
   }
   is_quads = primitive == MESA_PRIM_QUADS;
   if ((!is_quads && (primitive != MESA_PRIM_TRIANGLES || count < 3 ||
                      count > 36 || count % 3)) ||
       (is_quads && (count < 4 || count > 24 || count % 4))) {
      char message[96];
      wsprintfA(message, "skip: primitive=%u count=%u", primitive, count);
      nvx_bridge_trace(message);
      return 0;
   }
   triangle_count = is_quads ? (count / 4) * 2 : count / 3;
   InterlockedExchange(&nvx_bridge_last_primitive, (LONG)primitive);
   InterlockedExchange(&nvx_bridge_last_count, (LONG)count);
   InterlockedExchange(&nvx_bridge_last_triangle_count, (LONG)triangle_count);
   hdc = (HDC)InterlockedCompareExchangePointer(
      (PVOID volatile *)&nvx_wgl_hdc, NULL, NULL);
   if (!hdc) {
      nvx_bridge_trace("reject: no WGL HDC");
      return 0;
   }
   if (!sp->vs || !sp->vs->draw_data || sp->gs) {
      nvx_bridge_trace("reject: missing VS metadata or geometry shader");
      return 0;
   }
   depth_test = sp->depth_stencil && sp->depth_stencil->depth_enabled;
   depth_mode = depth_test ? 5U : 0U;
   if (depth_test) {
      char depth_mode_override[8];
      if (GetEnvironmentVariableA("NVX_MESA_DEPTH_MODE",
                                  depth_mode_override,
                                  sizeof(depth_mode_override)) &&
          depth_mode_override[1] == '\0' &&
          (depth_mode_override[0] == '2' ||
           depth_mode_override[0] == '3')) {
         depth_mode = (unsigned)(depth_mode_override[0] - '0');
         nvx_bridge_trace(depth_mode == 2U ?
            "diagnostic: zeta target attach only" :
            "diagnostic: depth compare without depth writes");
      }
   }
   {
      char disable_depth[8];
      if (GetEnvironmentVariableA("NVX_MESA_BRIDGE_DISABLE_DEPTH",
                                  disable_depth, sizeof(disable_depth)) &&
          disable_depth[0] == '1') {
         depth_test = 0;
         depth_mode = 0;
         nvx_bridge_trace("diagnostic: depth test disabled by test override");
      }
   }
   if (depth_test) {
      InterlockedExchange(&nvx_bridge_depth_stage_count, 0);
      memset(nvx_bridge_depth_stages, 0, sizeof(nvx_bridge_depth_stages));
   }
   if ((sp->rasterizer && sp->rasterizer->cull_face) ||
       (sp->depth_stencil &&
        (sp->depth_stencil->alpha_enabled ||
         sp->depth_stencil->stencil[0].enabled ||
         sp->depth_stencil->stencil[1].enabled ||
         sp->depth_stencil->depth_bounds_test))) {
      nvx_bridge_trace("reject: culling, alpha, stencil, or depth-bounds state");
      return 0;
   }
   if (depth_test &&
       (!sp->depth_stencil->depth_writemask ||
        sp->depth_stencil->depth_func != PIPE_FUNC_LESS ||
        !sp->zsbuf_cache ||
        (sp->framebuffer.zsbuf.format != PIPE_FORMAT_Z24_UNORM_S8_UINT &&
         sp->framebuffer.zsbuf.format != PIPE_FORMAT_Z24X8_UNORM &&
         sp->framebuffer.zsbuf.format != PIPE_FORMAT_X8Z24_UNORM) ||
        sp->zsbuf_cache->clear_val != util_pack64_z_stencil(
           sp->framebuffer.zsbuf.format, 1.0, 0))) {
      nvx_bridge_trace("reject: depth requires writable LESS Z24/Z32F and clear=1");
      return 0;
   }
   if (!sp->framebuffer.nr_cbufs || !sp->cbuf_cache[0]) {
      nvx_bridge_trace("reject: no softpipe color buffer/cache");
      return 0;
   }

   vinfo = &sp->vertex_info;
   viewport = &sp->viewports[0];
   if (vinfo->num_attribs != 2 ||
       vinfo->attrib[0].emit != EMIT_4F ||
       vinfo->attrib[1].emit != EMIT_4F) {
      nvx_bridge_trace("reject: unsupported vertex layout");
      return 0;
   }
   position_output = nvx_find_output(sp->vs->draw_data,
                                     TGSI_SEMANTIC_POSITION);
   color_output = nvx_find_output(sp->vs->draw_data,
                                  TGSI_SEMANTIC_COLOR);
   if (position_output < 0 || color_output < 0 ||
       vinfo->attrib[0].src_index != (unsigned)position_output ||
       vinfo->attrib[1].src_index != (unsigned)color_output) {
      nvx_bridge_trace("reject: shader outputs do not map position/color");
      return 0;
   }

   /* Tile the viewport in the miniport's proven 64x16 block-linear target.
    * Keep the viewport aligned so each GPU tile maps to one softpipe cache. */
   if (viewport->scale[0] == 0.0f || viewport->scale[1] == 0.0f ||
       viewport->scale[2] == 0.0f ||
       viewport->scale[0] != viewport->scale[0] ||
       viewport->scale[1] != viewport->scale[1] ||
       viewport->scale[2] != viewport->scale[2]) {
      nvx_bridge_trace("reject viewport: unsupported scale");
      return 0;
   }
   viewport_abs_x = viewport->scale[0] < 0.0f ?
      -viewport->scale[0] : viewport->scale[0];
   viewport_abs_y = viewport->scale[1] < 0.0f ?
      -viewport->scale[1] : viewport->scale[1];
   viewport_width = (unsigned)(viewport_abs_x * 2.0f + 0.5f);
   viewport_height = (unsigned)(viewport_abs_y * 2.0f + 0.5f);
   viewport_x = (int)(viewport->translate[0] - viewport_abs_x + 0.5f);
   viewport_y = (int)(viewport->translate[1] - viewport_abs_y + 0.5f);
   if (!viewport_width || !viewport_height || viewport_width > 4096 ||
       viewport_height > 4096 || viewport_x < 0 || viewport_y < 0 ||
       viewport_x + (int)viewport_width > (int)sp->framebuffer.width ||
       viewport_y + (int)viewport_height > (int)sp->framebuffer.height) {
      nvx_bridge_trace("reject viewport: dimensions, alignment, or bounds");
      return 0;
   }
   tile_start_x = viewport_x & ~63;
   tile_start_y = viewport_y & ~15;
   tile_end_x = (viewport_x + (int)viewport_width + 63) & ~63;
   tile_end_y = (viewport_y + (int)viewport_height + 15) & ~15;
   full_target = ((tile_end_x - tile_start_x) / 64) *
                 ((tile_end_y - tile_start_y) / 16) > 64;
   /* Clip each draw to its GL viewport inside the enclosing 64x16 GPU tile.
    * The bridge's target clear supplies the framebuffer's active clear color
    * for the untouched pixels in that tile. */

   for (i = 0; i < 4; i++)
      if (!nvx_float01(sp->cbuf_cache[0]->clear_color.f[i])) {
         nvx_bridge_trace("reject: framebuffer clear color is not normalized");
         return 0;
      }
   if (!nvx_pack_color(sp->cbuf_cache[0]->clear_color.f, &background)) {
      nvx_bridge_trace("reject: framebuffer clear color cannot be packed");
      return 0;
   }

   memset(&caps, 0, sizeof(caps));
   stage = "3D capability query";
   caps_escape = ExtEscape(hdc, NVX_ESCAPE_3D_QUERY_CAPS, 0, NULL,
                           sizeof(caps), (LPSTR)&caps);
   if (!caps_escape ||
       caps.Version != NVX_3D_ABI_VERSION ||
       (caps.Features & (NVX_3D_FEATURE_USER_ALLOC |
                         NVX_3D_FEATURE_UPLOAD |
                         NVX_3D_FEATURE_FIXED_PROFILE_SUBMIT)) !=
          (NVX_3D_FEATURE_USER_ALLOC |
           NVX_3D_FEATURE_UPLOAD |
           NVX_3D_FEATURE_FIXED_PROFILE_SUBMIT)) {
      char message[96];
      wsprintfA(message, "reject caps: escape=%d version=%u features=%08lX",
                caps_escape, caps.Version, (unsigned long)caps.Features);
      nvx_bridge_trace(message);
      return 0;
   }

   for (triangle = 0; triangle < triangle_count; triangle++) {
      unsigned index[3];
      if (is_quads) {
         unsigned base = (triangle / 2) * 4;
         if ((triangle & 1) == 0) {
            index[0] = base; index[1] = base + 1; index[2] = base + 2;
         } else {
            index[0] = base; index[1] = base + 2; index[2] = base + 3;
         }
      } else {
         index[0] = triangle * 3;
         index[1] = triangle * 3 + 1;
         index[2] = triangle * 3 + 2;
      }
      for (i = 0; i < 3; i++) {
         if (indices) index[i] = indices[index[i]];
         v[i] = (const float *)((const char *)vertices + index[i] * stride);
      }
      memcpy(rgba, v[0] + 4, sizeof(rgba));
      if (!nvx_pack_color(rgba, &color) ||
          memcmp(v[1] + 4, rgba, sizeof(rgba)) != 0 ||
          memcmp(v[2] + 4, rgba, sizeof(rgba)) != 0) {
         nvx_bridge_trace("reject: triangle color varies or is not normalized");
         return 0;
      }
      triangles[stored].color = color;
      for (i = 0; i < 3; i++) {
         const float *position = v[i];
         if (position[0] != position[0] || position[1] != position[1] ||
             position[2] != position[2] || position[0] < -8192.0f ||
             position[0] > 8192.0f || position[1] < -8192.0f ||
             position[1] > 8192.0f) {
            nvx_bridge_trace("reject: invalid window-space vertex");
            return 0;
         }
         triangles[stored].v[i].x = viewport_x +
            ((position[0] - viewport->translate[0]) / viewport->scale[0] +
             1.0f) * viewport_width * 0.5f;
         triangles[stored].v[i].y = viewport_y +
            ((position[1] - viewport->translate[1]) / viewport->scale[1] +
             1.0f) * viewport_height * 0.5f;
         triangles[stored].v[i].z =
            (position[2] - viewport->translate[2]) / viewport->scale[2];
         if (!nvx_float01((triangles[stored].v[i].z + 1.0f) * 0.5f)) {
            nvx_bridge_trace("reject: vertex depth outside clip volume");
            return 0;
         }
      }
      stored++;
   }

   if (full_target) {
      if (!nvx_bridge_full_target(sp, hdc, triangles, stored, background,
                                  viewport_width, viewport_height, depth_test,
                                  depth_mode))
         return 0;
      InterlockedIncrement(&nvx_bridge_gpu_batch_count);
      return 1;
   }

   memset(&target, 0, sizeof(target));
   memset(&target_result, 0, sizeof(target_result));
   target.Size = sizeof(target);
   target.Version = NVX_3D_ABI_VERSION;
   target.Usage = 1;
   if (depth_test)
      target.Usage |= NVX_3D_USAGE_DEPTH_TARGET;
   target.Bytes = depth_test ? NVX_3D_DEPTH_TARGET_TOTAL_BYTES :
                               NVX_3D_TRIANGLE_TARGET_BYTES;
   target.ClientToken = nvx_bridge_winsys.ClientToken;
   if (depth_test) {
      char message[96];
      char zf32_diagnostic[8];
      if (GetEnvironmentVariableA("NVX_MESA_ZF32_DIAGNOSTIC",
                                  zf32_diagnostic,
                                  sizeof(zf32_diagnostic)) &&
          zf32_diagnostic[0] == '1') {
         target.Usage |= NVX_3D_USAGE_DEPTH_ZF32;
         wsprintfA(message, "depth hardware diagnostic=ZF32/kind-6 format=%u",
                   (unsigned)sp->framebuffer.zsbuf.format);
         nvx_bridge_trace(message);
      } else if (sp->framebuffer.zsbuf.format == PIPE_FORMAT_Z24X8_UNORM) {
         /* NIL maps Z24X8 to SET_ZT_FORMAT_X8Z24 and PTE kind S8Z24. */
         target.Usage |= NVX_3D_USAGE_DEPTH_S8Z24;
      } else if (sp->framebuffer.zsbuf.format ==
                 PIPE_FORMAT_Z24_UNORM_S8_UINT) {
         /* This packed layout uses SET_ZT_FORMAT_S8Z24 with the same PTE kind. */
         target.Usage |= NVX_3D_USAGE_DEPTH_S8Z24 |
                         NVX_3D_USAGE_DEPTH_METHOD_S8Z24;
      }
      wsprintfA(message, "depth format=%u usage=%u",
                (unsigned)sp->framebuffer.zsbuf.format, target.Usage);
      nvx_bridge_trace(message);
   }
   stage = "target allocation";
   if (!nvx_alloc(hdc, &target, &target_result))
      goto done;

   memset(&vbo, 0, sizeof(vbo));
   memset(&vbo_result, 0, sizeof(vbo_result));
   vbo.Size = sizeof(vbo);
   vbo.Version = NVX_3D_ABI_VERSION;
   vbo.Usage = 1;
   vbo.Bytes = 4096;
   vbo.ClientToken = nvx_bridge_winsys.ClientToken;
   stage = "vertex buffer allocation";
   if (!nvx_alloc(hdc, &vbo, &vbo_result))
      goto done;

   for (tile_y = tile_start_y; tile_y < tile_end_y; tile_y += 16) {
      for (tile_x = tile_start_x; tile_x < tile_end_x; tile_x += 64) {
         unsigned batch_count = 0;
         memset(batch_vertices, 0, sizeof(batch_vertices));
         stage = "target clear";
         if (!nvx_clear_target(hdc, target_result.BufferId, background))
            goto done;
         stage = "depth clear";
         if (depth_test && !nvx_clear_depth(hdc, target_result.BufferId))
            goto done;
         for (triangle = 0; triangle < stored; triangle++) {
            float clip_left = tile_x > viewport_x ? (float)tile_x :
                                                       (float)viewport_x;
            float clip_top = tile_y > viewport_y ? (float)tile_y :
                                                     (float)viewport_y;
            float viewport_right = viewport_x + (float)viewport_width;
            float viewport_bottom = viewport_y + (float)viewport_height;
            float clip_right = tile_x + 64.0f < viewport_right ?
               tile_x + 64.0f : viewport_right;
            float clip_bottom = tile_y + 16.0f < viewport_bottom ?
               tile_y + 16.0f : viewport_bottom;
            if (clip_right > sp->framebuffer.width)
               clip_right = (float)sp->framebuffer.width;
            if (clip_bottom > sp->framebuffer.height)
               clip_bottom = (float)sp->framebuffer.height;
            unsigned vertices_in_polygon = nvx_clip_triangle_to_tile(
               &triangles[triangle], clip_left, clip_top, clip_right,
               clip_bottom, clipped);
            unsigned fan;
            for (fan = 1; fan + 1 < vertices_in_polygon; fan++) {
               struct nvx_bridge_vertex fan_vertices[3];
               unsigned j;
               fan_vertices[0] = clipped[0];
               fan_vertices[1] = clipped[fan];
               fan_vertices[2] = clipped[fan + 1];
               for (j = 0; j < 3; j++) {
                  float local_x = (fan_vertices[j].x - tile_x) / 64.0f;
                  float local_y = (fan_vertices[j].y - tile_y) / 16.0f;
                  float local_z = (fan_vertices[j].z + 1.0f) * 0.5f;
                  if (!nvx_float01(local_x) || !nvx_float01(local_y) ||
                      !nvx_float01(local_z)) {
                     nvx_bridge_trace("reject: clipped tile vertex out of range");
                     goto done;
                  }
                  if (depth_test) {
                     unsigned base = j * 8;
                     /* The shader multiplies XY by 64x16. Supply NDC/64 so
                      * the viewport maps clip coordinates to tile pixels. */
                     hw_vertices[base + 0] = (2.0f * local_x - 1.0f) / 64.0f;
                     hw_vertices[base + 1] = (2.0f * local_y - 1.0f) / 16.0f;
                     hw_vertices[base + 2] = local_z;
                     hw_vertices[base + 3] = 1.0f;
                     hw_vertices[base + 4] = local_z;
                     hw_vertices[base + 5] = 1.0f;
                     hw_vertices[base + 6] = 0.0f;
                     hw_vertices[base + 7] = 1.0f;
                  } else {
                     unsigned base = j * 6;
                     hw_vertices[base + 0] = (2.0f * local_x - 1.0f) / 64.0f;
                     hw_vertices[base + 1] = (2.0f * local_y - 1.0f) / 16.0f;
                     /* Color-only draws use the original XYUVRQ shader ABI. */
                     hw_vertices[base + 2] = local_z;
                     hw_vertices[base + 3] = 0.0f;
                     hw_vertices[base + 4] = 0.0f;
                     hw_vertices[base + 5] = 1.0f;
                  }
               }
               stage = "triangle submit/fence";
               if (depth_test) {
                  if (!nvx_submit_triangle(hdc, target_result.BufferId,
                                           vbo_result.BufferId, hw_vertices,
                                           triangles[triangle].color, 1)) {
                     nvx_bridge_trace("submit failed: depth draw or fence failed");
                     goto done;
                  }
               } else {
                  unsigned batch;
                  for (batch = 0; batch < batch_count; batch++)
                     if (batch_colors[batch] == triangles[triangle].color &&
                         batch_vertices[batch] + 3 <=
                            NVX_3D_MAX_BATCH_VERTICES)
                        break;
                  if (batch == batch_count && batch_count < 12) {
                     batch_colors[batch] = triangles[triangle].color;
                     batch_count++;
                  }
                  if (batch < batch_count &&
                      batch_vertices[batch] + 3 <=
                         NVX_3D_MAX_BATCH_VERTICES) {
                     memcpy(&batches[batch][batch_vertices[batch] * 6],
                            hw_vertices, 3 * 6 * sizeof(float));
                     batch_vertices[batch] += 3;
                  } else if (!nvx_submit_triangle(hdc, target_result.BufferId,
                              vbo_result.BufferId, hw_vertices,
                              triangles[triangle].color, 0)) {
                     nvx_bridge_trace("submit failed: fallback triangle or fence failed");
                     goto done;
                  }
               }
               if (depth_test && tile_x == tile_start_x &&
                   tile_y == tile_start_y) {
                  char depth_message[128];
                  unsigned expected = triangles[triangle].color;
                  if (!nvx_readback_target(hdc, target_result.BufferId,
                                           pixels)) {
                     nvx_bridge_trace("depth diagnostic intermediate readback failed");
                     goto done;
                  }
                  wsprintfA(depth_message,
                     "depth stage triangle=%u color=%08lx pixel=%08lx",
                     triangle, (unsigned long)expected,
                     (unsigned long)pixels[8 * 64 + 32]);
                  {
                     LONG stage_index=InterlockedIncrement(
                        &nvx_bridge_depth_stage_count)-1;
                     if(stage_index>=0&&stage_index<3)
                        memcpy(nvx_bridge_depth_stages[stage_index],
                               depth_message,strlen(depth_message)+1);
                  }
                  nvx_bridge_trace(depth_message);
               }
            }
         }
         for (triangle = 0; triangle < batch_count; triangle++) {
            if (!batch_vertices[triangle])
               continue;
            if (!nvx_submit_triangles(hdc, target_result.BufferId,
                  vbo_result.BufferId, batches[triangle],
                  batch_vertices[triangle], batch_colors[triangle])) {
               nvx_bridge_trace("submit failed: batched triangles or fence failed");
               goto done;
            }
         }
         stage = "GPU target readback";
         if (!nvx_readback_target(hdc, target_result.BufferId, pixels)) {
            nvx_bridge_trace("submit failed: final GPU target readback failed");
            goto done;
         }
         if (depth_test && tile_x == tile_start_x &&
             tile_y == tile_start_y) {
            char depth_message[128];
            unsigned green = 0, red = 0, black = 0;
            for (i = 0; i < NVX_3D_READBACK_PIXELS; i++) {
               unsigned r = (pixels[i] >> 16) & 0xff;
               unsigned g = (pixels[i] >> 8) & 0xff;
               unsigned b = pixels[i] & 0xff;
               if (g > 200 && r < 32 && b < 32) green++;
               else if (r > 200 && g < 32 && b < 32) red++;
               else if (r < 16 && g < 16 && b < 16) black++;
            }
            wsprintfA(depth_message,
                      "depth GPU tile=%u,%u green=%u red=%u black=%u center=%08lx",
                      (unsigned)tile_x, (unsigned)tile_y, green, red, black,
                      (unsigned long)pixels[8 * 64 + 32]);
            nvx_bridge_trace(depth_message);
         }
         stage = "softpipe tile update";
         if (!nvx_update_softpipe_tile(sp, pixels, tile_x, tile_y, 0)) {
            nvx_bridge_trace("submit passed but softpipe tile update failed");
            goto done;
         }
      }
   }
   nvx_bridge_trace("submit passed: multi-triangle pixels copied after GPU fences (tiled)");
   InterlockedIncrement(&nvx_bridge_gpu_batch_count);
   ok = 1;
done:
   if (!ok) {
      char message[96];
      wsprintfA(message, "bridge failed at %s", stage);
      nvx_bridge_trace(message);
   }
   nvx_free(hdc, vbo_result.BufferId);
   nvx_free(hdc, target_result.BufferId);
   return ok;
}
