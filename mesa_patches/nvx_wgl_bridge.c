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

static HDC nvx_wgl_hdc;
static volatile LONG nvx_bridge_trace_count;
static volatile LONG nvx_bridge_screen_count;
static volatile LONG nvx_bridge_callback_count;
static volatile LONG nvx_bridge_gpu_batch_count;
static char nvx_bridge_last_message[96];
static LONG nvx_bridge_depth_stage_count;
static char nvx_bridge_depth_stages[3][96];
static void nvx_bridge_trace(const char *message);

struct nvx_bridge_status {
   uint32_t size;
   uint32_t version;
   LONG screen_count;
   LONG callback_count;
   LONG gpu_batch_count;
   char last_message[96];
   LONG depth_stage_count;
   char depth_stages[3][96];
};

__declspec(dllexport) BOOL WINAPI
NvxQueryBridgeStatus(struct nvx_bridge_status *status, DWORD bytes);

__declspec(dllexport) BOOL WINAPI
NvxQueryBridgeStatus(struct nvx_bridge_status *status, DWORD bytes)
{
   if (!status || bytes < sizeof(*status))
      return FALSE;
   status->size = sizeof(*status);
   status->version = 1;
   status->screen_count = InterlockedCompareExchange(&nvx_bridge_screen_count, 0, 0);
   status->callback_count = InterlockedCompareExchange(&nvx_bridge_callback_count, 0, 0);
   status->gpu_batch_count = InterlockedCompareExchange(&nvx_bridge_gpu_batch_count, 0, 0);
   memcpy(status->last_message, nvx_bridge_last_message,
          sizeof(status->last_message));
   status->depth_stage_count = InterlockedCompareExchange(
      &nvx_bridge_depth_stage_count, 0, 0);
   memcpy(status->depth_stages, nvx_bridge_depth_stages,
          sizeof(status->depth_stages));
   return TRUE;
}

void nvx_wgl_set_hdc(HDC hdc);
int nvx_wgl_try_softpipe_triangle(struct softpipe_context *sp,
                                  const void *vertices,
                                  const uint16_t *indices, unsigned stride,
                                  unsigned primitive, unsigned count);

void
nvx_wgl_set_hdc(HDC hdc)
{
   InterlockedExchangePointer((PVOID volatile *)&nvx_wgl_hdc, hdc);
   InterlockedIncrement(&nvx_bridge_screen_count);
   nvx_bridge_trace("bridge active: WGL screen created");
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

   if (count > 32)
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
   struct {
      NVX_3D_UPLOAD_BUFFER header;
      float vertices[24];
   } request;

   memset(&request, 0, sizeof(request));
   request.header.Size = sizeof(request.header);
   request.header.Version = NVX_3D_ABI_VERSION;
   request.header.BufferId = buffer_id;
   if (vertex_bytes != 72 && vertex_bytes != 96)
      return 0;
   request.header.Bytes = vertex_bytes;
   memcpy(request.vertices, vertices, vertex_bytes);
   return ExtEscape(hdc, NVX_ESCAPE_3D_UPLOAD,
                    sizeof(request.header) + vertex_bytes,
                    (LPCSTR)&request, 0, NULL) != 0;
}

static void
nvx_free(HDC hdc, uint64_t buffer_id)
{
   NVX_3D_FREE_BUFFER request;

   if (!buffer_id)
      return;
   request.Size = sizeof(request);
   request.Version = NVX_3D_ABI_VERSION;
   request.BufferId = buffer_id;
   ExtEscape(hdc, NVX_ESCAPE_3D_FREE, sizeof(request),
             (LPCSTR)&request, 0, NULL);
}

static int
nvx_wait_fence(HDC hdc, uint64_t fence_value)
{
   NVX_3D_WAIT_FENCE wait, waited;
   memset(&wait, 0, sizeof(wait));
   wait.Size = sizeof(wait);
   wait.Version = NVX_3D_ABI_VERSION;
   wait.FenceValue = fence_value;
   wait.TimeoutMs = 10000;
   memset(&waited, 0, sizeof(waited));
   return ExtEscape(hdc, NVX_ESCAPE_3D_WAIT, sizeof(wait),
                    (LPCSTR)&wait, sizeof(waited), (LPSTR)&waited) != 0 &&
          waited.Status == NVX_3D_STATUS_SUCCESS &&
          waited.FenceValue == fence_value;
}

static int
nvx_clear_target(HDC hdc, uint64_t target_id, uint32_t color)
{
   NVX_3D_SUBMIT submit, submitted;
   memset(&submit, 0, sizeof(submit));
   submit.Size = sizeof(submit);
   submit.Version = NVX_3D_ABI_VERSION;
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
nvx_clear_depth(HDC hdc, uint64_t target_id)
{
   NVX_3D_SUBMIT submit, submitted;
   memset(&submit, 0, sizeof(submit));
   submit.Size = sizeof(submit);
   submit.Version = NVX_3D_ABI_VERSION;
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
                         int x0, int y0)
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
   if (x0 < 0 || y0 < 0 || x0 + 64 > (int)sp->framebuffer.width ||
       y0 + 16 > (int)sp->framebuffer.height) {
      nvx_bridge_trace("reject viewport: target outside framebuffer bounds");
      return 0;
   }
   if (x0 & 63) {
      nvx_bridge_trace("reject viewport: target x is not tile aligned");
      return 0;
   }
   for (y = 0; y < 16; y++) {
      for (x = 0; x < 64; x++) {
         uint32_t pixel = pixels[y * 64 + x];
         struct softpipe_cached_tile *tile =
            sp_get_cached_tile(sp->cbuf_cache[0], x0 + x, y0 + y, 0);
         float *rgba;
         if (!tile) {
            nvx_bridge_trace("reject framebuffer: tile lookup returned null");
            return 0;
         }
         rgba = tile->data.color[(y0 + y) & (TILE_SIZE - 1)]
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

struct nvx_bridge_vertex {
   float x, y, z;
};

struct nvx_bridge_triangle {
   struct nvx_bridge_vertex v[3];
   uint32_t color;
};

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
                          float left, float top,
                          struct nvx_bridge_vertex output[12])
{
   struct nvx_bridge_vertex a[12], b[12];
   unsigned count = 3;

   memcpy(a, triangle->v, sizeof(triangle->v));
   count = nvx_clip_axis(a, count, b, 0, left, 1);
   count = nvx_clip_axis(b, count, a, 0, left + 64.0f, 0);
   count = nvx_clip_axis(a, count, b, 1, top, 1);
   count = nvx_clip_axis(b, count, a, 1, top + 16.0f, 0);
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
   int is_quads, depth_test;
   int ok = 0;
   int caps_escape;
   const char *stage = "bridge validation";
   char enabled[8];

   if (!GetEnvironmentVariableA("NVX_MESA_GALLIUM_BRIDGE", enabled,
                                sizeof(enabled)) || enabled[0] != '1')
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
   if (!viewport_width || !viewport_height || viewport_width % 64 ||
       viewport_height % 16 || viewport_width > 4096 ||
       viewport_height > 4096 || (viewport_x & 63) || viewport_x < 0 ||
       viewport_y < 0 ||
       viewport_x + (int)viewport_width > (int)sp->framebuffer.width ||
       viewport_y + (int)viewport_height > (int)sp->framebuffer.height) {
      nvx_bridge_trace("reject viewport: dimensions, alignment, or bounds");
      return 0;
   }

   for (i = 0; i < 4; i++)
      if (!nvx_float01(sp->cbuf_cache[0]->clear_color.f[i])) {
         nvx_bridge_trace("reject: framebuffer clear color is not normalized");
         return 0;
      }
   if (sp->cbuf_cache[0]->clear_color.f[0] == 0.0f &&
       sp->cbuf_cache[0]->clear_color.f[1] == 0.0f &&
       sp->cbuf_cache[0]->clear_color.f[2] == 0.0f)
      background = 0;
   else if (sp->cbuf_cache[0]->clear_color.f[0] == 1.0f &&
            sp->cbuf_cache[0]->clear_color.f[1] == 1.0f &&
            sp->cbuf_cache[0]->clear_color.f[2] == 1.0f)
      background = 0xffffffffU;
   else {
      nvx_bridge_trace("reject: GPU clear supports only black or white");
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
                         NVX_3D_FEATURE_SUBMIT)) !=
          (NVX_3D_FEATURE_USER_ALLOC |
           NVX_3D_FEATURE_UPLOAD |
           NVX_3D_FEATURE_SUBMIT)) {
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

   memset(&target, 0, sizeof(target));
   memset(&target_result, 0, sizeof(target_result));
   target.Size = sizeof(target);
   target.Version = NVX_3D_ABI_VERSION;
   target.Usage = 1;
   if (depth_test)
      target.Usage |= NVX_3D_USAGE_DEPTH_TARGET;
   target.Bytes = depth_test ? NVX_3D_DEPTH_TARGET_TOTAL_BYTES :
                               NVX_3D_TRIANGLE_TARGET_BYTES;
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
   if (!ExtEscape(hdc, NVX_ESCAPE_3D_ALLOC, sizeof(target),
                  (LPCSTR)&target, sizeof(target_result),
                  (LPSTR)&target_result))
      goto done;

   memset(&vbo, 0, sizeof(vbo));
   memset(&vbo_result, 0, sizeof(vbo_result));
   vbo.Size = sizeof(vbo);
   vbo.Version = NVX_3D_ABI_VERSION;
   vbo.Usage = 1;
   vbo.Bytes = 4096;
   stage = "vertex buffer allocation";
   if (!ExtEscape(hdc, NVX_ESCAPE_3D_ALLOC, sizeof(vbo),
                  (LPCSTR)&vbo, sizeof(vbo_result),
                  (LPSTR)&vbo_result))
      goto done;

   for (tile_y = viewport_y;
        tile_y < viewport_y + (int)viewport_height; tile_y += 16) {
      for (tile_x = viewport_x;
           tile_x < viewport_x + (int)viewport_width; tile_x += 64) {
         stage = "target clear";
         if (!nvx_clear_target(hdc, target_result.BufferId, background))
            goto done;
         stage = "depth clear";
         if (depth_test && !nvx_clear_depth(hdc, target_result.BufferId))
            goto done;
         for (triangle = 0; triangle < stored; triangle++) {
            unsigned vertices_in_polygon = nvx_clip_triangle_to_tile(
               &triangles[triangle], (float)tile_x, (float)tile_y, clipped);
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
                     hw_vertices[base + 4] = 0.0f;
                     hw_vertices[base + 5] = 0.0f;
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
               if (!nvx_submit_triangle(hdc, target_result.BufferId,
                                        vbo_result.BufferId, hw_vertices,
                                        triangles[triangle].color,
                                        depth_test)) {
                  nvx_bridge_trace("submit failed: preserve draw or fence failed");
                  goto done;
               }
               if (depth_test && tile_x == viewport_x &&
                   tile_y == viewport_y) {
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
         stage = "GPU target readback";
         if (!nvx_readback_target(hdc, target_result.BufferId, pixels)) {
            nvx_bridge_trace("submit failed: final GPU target readback failed");
            goto done;
         }
         if (depth_test && tile_x == viewport_x && tile_y == viewport_y) {
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
         if (!nvx_update_softpipe_tile(sp, pixels, tile_x, tile_y)) {
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
