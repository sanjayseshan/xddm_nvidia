/*
 * XP replacement for the libdrm entry points used by Nouveau.
 *
 * Device/version discovery is backed by a registered NVX adapter. GPU
 * commands remain unsupported until they have operation-specific validated
 * NVX implementations; this layer never forwards arbitrary DRM ioctls.
 */
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "xf86drm.h"
#include "drm-uapi/nouveau_drm.h"
#include "nvif/class.h"
#include "nvif/cl0080.h"
#include "nvif/ioctl.h"

#if defined(_WIN32) || defined(_WIN64)
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
#endif

#define NVX_DRM_DEVICE_SLOTS 8U
#define NVX_DRM_BUFFER_SLOTS 2048U
#define NVX_DRM_OBJECT_SLOTS 8U
#define NVX_DRM_MAX_SCLASS 16U
#define NVX_DRM_FD_BASE 0x4e580100
#define NVX_MESA_CLASS_NVE4_P2MF 0x0000a040U
#define NVX_RM_CLASS_KEPLER_I2M_B 0x0000a140U
#define NVX_RM_CLASS_AMPERE_DMA_COPY_B 0x0000c7b5U
#define NVX_RM_CLASS_FERMI_2D_A 0x0000902dU
#define NVX_RM_CLASS_AMPERE_COMPUTE_B 0x0000c7c0U
#define NVX_RM_OBJECT_NVC0_M2MF 0xc6970010U
#define NVX_RM_OBJECT_NVC0_COPY 0xc6970011U
#define NVX_RM_OBJECT_NVC0_2D 0xc6970012U

struct nvx_drm_object_slot {
   uint64_t user_object;
   uint32_t user_handle;
   uint32_t user_class;
   uint32_t rm_handle;
   uint32_t rm_class;
};

struct nvx_drm_bo_slot {
   uint32_t handle;
   NVX_NOUVEAU_BO bo;
};

struct nvx_drm_device_slot {
   int fd;
   NVX_WINSYS_DEVICE device;
   uint64_t client_token;
   uint32_t next_handle;
#if defined(_WIN32) || defined(_WIN64)
   uint64_t shader_address[6];
   uint64_t shader_triangle_vertex_address;
   uint64_t shader_triangle_input_vertex_address;
   uint64_t shader_triangle_target_address;
   uint64_t shader_ffvs_target_address;
   uint64_t shader_full_target_address;
   uint64_t shader_cube_vertex_address;
   uint32_t shader_cube_vertex_stride;
   uint32_t shader_cube_vertex_count;
   uint32_t shader_cube_vertex_flush_done;
   uint32_t shader_cube_vertex_readback_scheduled;
   uint32_t shader_address_mask;
   uint32_t shader_draw_snapshot_done;
   uint32_t shader_triangle_snapshot_done;
   uint32_t shader_triangle_input_snapshot_done;
   uint32_t shader_triangle_input_readback_scheduled;
   uint32_t shader_triangle_input_flush_trace_done;
   uint32_t shader_ffvs_snapshot_done;
   uint32_t gr_state[0x4000U / sizeof(uint32_t)];
   uint32_t gr_draw_state_snapshot_done;
   uint32_t gr_ffvs_draw_state_snapshot_done;
#endif
   struct nvx_drm_bo_slot buffers[NVX_DRM_BUFFER_SLOTS];
   struct nvx_drm_object_slot objects[NVX_DRM_OBJECT_SLOTS];
};

static struct nvx_drm_device_slot nvx_drm_devices[NVX_DRM_DEVICE_SLOTS];
static volatile int nvx_drm_lock;
static NVX_XF86DRM_STATUS nvx_drm_status;
#if defined(_WIN32) || defined(_WIN64)
static uint32_t nvx_drm_fence_trace_count;
static uint32_t nvx_drm_prep_trace_count;
static uint32_t nvx_drm_prep_call_trace_count;
static uint32_t nvx_drm_shadow_flush_trace_count;
static uint32_t nvx_drm_vertex_upload_trace_count;
#define NVX_DRM_VERTEX_DUMP_LIMIT 16U
static uint64_t nvx_drm_vertex_dump_addresses[NVX_DRM_VERTEX_DUMP_LIMIT];
static uint32_t nvx_drm_vertex_dump_count;
#define NVX_DRM_SHADER_DUMP_LIMIT 16U
#define NVX_DRM_SHADER_DUMP_BYTES 512U
#endif
static int nvx_drm_unsupported(void);
static int nvx_drm_unsupported_operation(uint32_t kind,
                                        unsigned long operation);
static int nvx_drm_lookup(int fd, NVX_WINSYS_DEVICE *device);
static void nvx_drm_lock_acquire(void);
static void nvx_drm_lock_release(void);
static int nvx_drm_nvif_object_del(int fd, void *data, unsigned long size);
static struct nvx_drm_device_slot *nvx_drm_find_device_locked(int fd);
static struct nvx_drm_bo_slot *nvx_drm_find_bo_locked(
   struct nvx_drm_device_slot *device, uint32_t handle);

#if defined(_WIN32) || defined(_WIN64)
#define NVX_DRM_VERTEX_DUMP_MAX_BYTES 1024U
#define NVX_DRM_VERTEX_DUMP_VERTICES 24U

struct nvx_drm_vertex_dump {
   uint64_t buffer_id;
   uint64_t gpu_va;
   uint32_t allocation_bytes;
   uint32_t offset;
   uint32_t bytes;
   uint32_t dwords[NVX_DRM_VERTEX_DUMP_MAX_BYTES / sizeof(uint32_t)];
   uint32_t copied_from_shadow;
   uint32_t force_gpu_readback;
};

struct nvx_drm_shader_dump {
   uint64_t buffer_id;
   uint64_t gpu_va;
   uint32_t stage;
   uint32_t draw_kind;
   uint32_t allocation_bytes;
   uint32_t offset;
   uint32_t bytes;
   uint32_t dwords[NVX_DRM_SHADER_DUMP_BYTES / sizeof(uint32_t)];
   uint32_t copied_from_shadow;
};

static uint32_t
nvx_drm_capture_shader_addresses_locked(
   struct nvx_drm_device_slot *device, const uint8_t *dword_data,
   uint32_t dword_count, struct nvx_drm_shader_dump *dumps)
{
   static const uint32_t trace_methods[] = {
      0x037cU, 0x0800U, 0x0804U, 0x0808U, 0x080cU, 0x0810U, 0x0814U,
      0x0a00U, 0x0a04U, 0x0a08U, 0x0a0cU, 0x0a10U, 0x0a14U, 0x0a18U,
      0x0c00U, 0x0c04U, 0x0c08U, 0x0c0cU, 0x0e00U, 0x0e04U, 0x0e08U,
      0x0f90U, 0x0ff4U, 0x0ff8U,
      0x0dacU, 0x0db0U, 0x1160U, 0x1164U, 0x121cU, 0x12ccU, 0x12d0U,
      0x12e4U, 0x12e8U, 0x1308U, 0x130cU, 0x135cU, 0x1360U, 0x153cU,
      0x1618U, 0x1918U, 0x191cU, 0x1920U, 0x192cU, 0x19c4U, 0x1a00U,
      0x1c00U, 0x1c04U, 0x1c08U, 0x1c0cU, 0x1c10U, 0x1c14U, 0x1c18U,
      0x0d74U, 0x2040U, 0x2054U, 0x2058U, 0x2080U, 0x2094U, 0x2098U,
      0x2140U, 0x2154U, 0x2158U, 0x3818U, 0x3820U, 0x3858U
   };
   uint32_t cursor = 0, captured = 0, draw_seen = 0, draw_kind = 0;
   uint32_t draw_primitive = 0xffffffffU, draw_vertex_count = 0;

   while (cursor < dword_count) {
      uint32_t header, packet_type, count, method, subchannel, i;
      memcpy(&header, dword_data + (size_t)cursor * 4U, sizeof(header));
      packet_type = header & 0xe0000000U;
      if (packet_type == 0x80000000U) {
         cursor++;
         continue;
      }
      if (packet_type != 0x20000000U && packet_type != 0x60000000U &&
          packet_type != 0xa0000000U)
         break;
      count = (header >> 16) & 0x1fffU;
      method = (header & 0x1fffU) << 2;
      subchannel = (header >> 13) & 7U;
      if (!count || count > dword_count - cursor - 1U)
         break;
      for (i = 0; i < count; i++) {
         uint32_t current_method, pair_index, stage, high, low;
         uint64_t address;
         const NVX_3D_NVC0_ADDRESS_PAIR *pair;
         if (packet_type == 0x20000000U)
            current_method = method + i * 4U;
         else if (packet_type == 0xa0000000U && i)
            current_method = method + 4U;
         else
            current_method = method;
         if (subchannel != 0U)
            continue;
         if (current_method < sizeof(device->gr_state))
            device->gr_state[current_method / sizeof(uint32_t)] =
               Nvx3dBatchReadDword(dword_data, cursor + 1U + i);
         if (current_method == 0x1618U) {
            draw_primitive = Nvx3dBatchReadDword(dword_data, cursor + 1U + i);
            draw_seen = draw_primitive == 7U || draw_primitive == 4U;
         } else if (current_method == 0x0d78U) {
            draw_vertex_count = Nvx3dBatchReadDword(dword_data, cursor + 1U + i);
         }
         if (Nvx3dNvc0AddressMethodKind(subchannel, current_method,
                                       &pair_index, &stage) != 1)
            continue;
         pair = &Nvx3dNvc0AddressPairs[pair_index];
         if (pair->HighMethod != 0x2014U || pair->IndexStride != 0x40U ||
             stage >= 6U || i + 1U >= count ||
             packet_type != 0x20000000U)
            continue;
         high = Nvx3dBatchReadDword(dword_data, cursor + 1U + i);
         low = Nvx3dBatchReadDword(dword_data, cursor + 2U + i);
         if (high > 0xffU)
            continue;
         address = ((uint64_t)high << 32) | low;
         device->shader_address[stage] = address;
         device->shader_address_mask |= 1U << stage;
      }
      cursor += count + 1U;
   }

   /* Capture the programmable VS+FS triangle, the fragment-only triangle,
    * and the later cube independently. */
   if (draw_seen && draw_primitive == 4U && draw_vertex_count == 3U &&
       !device->shader_triangle_snapshot_done) {
      draw_kind = 2U;
      device->shader_triangle_snapshot_done = 1U;
      device->shader_triangle_vertex_address = device->shader_address[1];
      device->shader_triangle_target_address =
         ((uint64_t)device->gr_state[0x0800U / 4U] << 32) |
         device->gr_state[0x0804U / 4U];
   } else if (draw_seen && draw_primitive == 4U && draw_vertex_count == 3U &&
       !device->shader_ffvs_snapshot_done && device->shader_address[1] &&
       device->shader_address[1] != device->shader_triangle_vertex_address) {
      draw_kind = 3U;
      device->shader_ffvs_snapshot_done = 1U;
      device->shader_ffvs_target_address =
         ((uint64_t)device->gr_state[0x0800U / 4U] << 32) |
         device->gr_state[0x0804U / 4U];
   } else if (draw_seen && draw_primitive == 7U && draw_vertex_count == 24U &&
       !device->gr_draw_state_snapshot_done &&
       (device->shader_address_mask & ((1U << 1) | (1U << 2) | (1U << 5))) ==
       ((1U << 1) | (1U << 2) | (1U << 5))) {
      draw_kind = 1U;
      device->gr_draw_state_snapshot_done = 1U;
      device->shader_cube_vertex_address =
         ((uint64_t)device->gr_state[0x1c04U / 4U] << 32) |
         device->gr_state[0x1c08U / 4U];
      device->shader_cube_vertex_stride =
         device->gr_state[0x1c00U / 4U] & 0x0fffU;
      device->shader_cube_vertex_count = draw_vertex_count;
   }
   if (draw_kind && (draw_kind != 3U ||
                     !device->gr_ffvs_draw_state_snapshot_done)) {
      uint32_t values[sizeof(trace_methods) / sizeof(trace_methods[0])];
      uint32_t i;
      for (i = 0; i < sizeof(trace_methods) / sizeof(trace_methods[0]); i++)
         values[i] = device->gr_state[trace_methods[i] / sizeof(uint32_t)];
      NvxWglRecordNouveauDrawState(trace_methods, values,
         (uint32_t)(sizeof(trace_methods) / sizeof(trace_methods[0])),
         draw_kind);
      if (draw_kind == 3U)
         device->gr_ffvs_draw_state_snapshot_done = 1U;
   }
   /* Keep the full-window back-buffer target from the first regular draw.
    * The test can then inspect the BO through CE readback, independently of
    * Mesa's glReadPixels path. The 0xa00-byte pitch filters out the small FBO
    * controls while matching the current 632x453 XP display target. */
   if (draw_seen && device->gr_state[0x0808U / 4U] >= 0x800U &&
       device->gr_state[0x080cU / 4U] >= 200U) {
      uint64_t target = ((uint64_t)device->gr_state[0x0800U / 4U] << 32) |
                        device->gr_state[0x0804U / 4U];
      if (target)
         device->shader_full_target_address = target;
   }
   /* The first GLSL triangle is the passing explicit-array control. Capture
    * the next GLSL triangle separately; the test submits its Begin/End
    * diagnostic with the same program and primitive, but a distinct VAO. */
   if (!draw_kind && draw_seen && draw_primitive == 4U &&
       draw_vertex_count == 3U && device->shader_triangle_snapshot_done &&
       !device->shader_triangle_input_snapshot_done) {
      uint32_t values[sizeof(trace_methods) / sizeof(trace_methods[0])];
      uint32_t i;
      for (i = 0; i < sizeof(trace_methods) / sizeof(trace_methods[0]); i++)
         values[i] = device->gr_state[trace_methods[i] / sizeof(uint32_t)];
      NvxWglRecordNouveauDrawState(trace_methods, values,
         (uint32_t)(sizeof(trace_methods) / sizeof(trace_methods[0])), 4U);
      device->shader_triangle_input_snapshot_done = 1U;
      device->shader_triangle_input_vertex_address =
         ((uint64_t)device->gr_state[0x1c04U / 4U] << 32) |
         device->gr_state[0x1c08U / 4U];
   }

   /* Program addresses are persistent 3D state. A later draw batch often
    * omits SP_ADDRESS writes, so snapshot the cached bindings at draw time. */
   if (!draw_kind ||
       (draw_kind == 1U && device->shader_draw_snapshot_done) ||
       (draw_kind == 2U && device->shader_triangle_snapshot_done == 0U) ||
       (draw_kind == 3U && device->shader_ffvs_snapshot_done == 0U) ||
       (draw_kind == 1U &&
        (device->shader_address_mask & ((1U << 1) | (1U << 2) | (1U << 5))) !=
        ((1U << 1) | (1U << 2) | (1U << 5))))
      return 0;
   for (uint32_t stage = 0; stage < 6U && captured < NVX_DRM_SHADER_DUMP_LIMIT;
        stage++) {
      uint64_t address = device->shader_address[stage];
      uint32_t slot_index;
      if (!address)
         continue;
      for (slot_index = 0; slot_index < NVX_DRM_BUFFER_SLOTS; slot_index++) {
         struct nvx_drm_bo_slot *slot = &device->buffers[slot_index];
         uint64_t base = slot->bo.allocation.GpuVa;
         uint64_t allocation_bytes = slot->bo.allocation.Bytes;
         uint64_t offset;
         struct nvx_drm_shader_dump *dump;
         if (!slot->handle || !slot->bo.allocation.BufferId || !base ||
             address < base || address - base >= allocation_bytes)
            continue;
         offset = address - base;
         if (offset > UINT32_MAX || allocation_bytes > UINT32_MAX)
            break;
         dump = &dumps[captured];
         memset(dump, 0, sizeof(*dump));
         dump->buffer_id = slot->bo.allocation.BufferId;
         dump->gpu_va = address;
         dump->stage = stage;
         dump->draw_kind = draw_kind;
         dump->allocation_bytes = (uint32_t)allocation_bytes;
         dump->offset = (uint32_t)offset;
         dump->bytes = NVX_DRM_SHADER_DUMP_BYTES;
         if (dump->bytes > allocation_bytes - offset)
            dump->bytes = (uint32_t)(allocation_bytes - offset) & ~3U;
         captured++;
         break;
      }
   }
   if (captured && draw_kind == 1U)
      device->shader_draw_snapshot_done = 1U;
   return captured;
}

/* Capture the first enabled NVC0 vertex array from this batch while BO
 * metadata and persistent CPU shadows are protected by nvx_drm_lock. */
static int
nvx_drm_capture_vertex_shadow_locked(
   struct nvx_drm_device_slot *device, const uint8_t *dword_data,
   uint32_t dword_count, const uint64_t *captured_addresses,
   uint32_t captured_count, struct nvx_drm_vertex_dump *dump)
{
   uint32_t cursor = 0;

   while (cursor < dword_count) {
      uint32_t header, packet_type, count, method, subchannel, i;
      memcpy(&header, dword_data + (size_t)cursor * 4U, sizeof(header));
      packet_type = header & 0xe0000000U;
      if (packet_type == 0x80000000U) {
         cursor++;
         continue;
      }
      if (packet_type != 0x20000000U && packet_type != 0x60000000U &&
          packet_type != 0xa0000000U)
         return 0;
      count = (header >> 16) & 0x1fffU;
      method = (header & 0x1fffU) << 2;
      subchannel = (header >> 13) & 7U;
      if (!count || count > dword_count - cursor - 1U)
         return 0;
      for (i = 0; i < count; i++) {
         uint32_t current_method, pair_index, array_index;
         uint32_t high, low, fetch, stride, bytes, slot_index;
         uint64_t address;
         const NVX_3D_NVC0_ADDRESS_PAIR *pair;
         if (packet_type == 0x20000000U)
            current_method = method + i * 4U;
         else if (packet_type == 0xa0000000U && i)
            current_method = method + 4U;
         else
            current_method = method;
         if (subchannel != 0U ||
             Nvx3dNvc0AddressMethodKind(subchannel, current_method,
                                        &pair_index, &array_index) != 1)
            continue;
         pair = &Nvx3dNvc0AddressPairs[pair_index];
         if (pair->HighMethod < 0x1c04U ||
             pair->HighMethod > 0x1df4U || pair->IndexStride != 0x10U ||
             current_method != pair->HighMethod +
                               array_index * pair->IndexStride || i == 0U ||
             packet_type != 0x20000000U)
            continue;
         fetch = Nvx3dBatchReadDword(dword_data, cursor + i);
         if (!(fetch & 0x00001000U))
            continue;
         stride = fetch & 0x00000fffU;
         if (!stride)
            continue;
         high = Nvx3dBatchReadDword(dword_data, cursor + 1U + i);
         low = Nvx3dBatchReadDword(dword_data, cursor + 2U + i);
         if (high > 0xffU)
            continue;
         address = ((uint64_t)high << 32) | low;
         {
            uint64_t immediate_position_address =
               ((uint64_t)device->gr_state[0x1c04U / 4U] << 32) |
               device->gr_state[0x1c08U / 4U];
            uint32_t force_gpu_readback =
               device->shader_triangle_input_snapshot_done &&
               device->shader_triangle_input_flush_trace_done &&
               !device->shader_triangle_input_readback_scheduled &&
               address == immediate_position_address;
         for (slot_index = 0; slot_index < captured_count; slot_index++)
            if (captured_addresses[slot_index] == address &&
                !force_gpu_readback)
               goto next_address;
         for (slot_index = 0; slot_index < NVX_DRM_BUFFER_SLOTS;
              slot_index++) {
            struct nvx_drm_bo_slot *slot = &device->buffers[slot_index];
            uint64_t base = slot->bo.allocation.GpuVa;
            uint64_t allocation_bytes = slot->bo.allocation.Bytes;
            uint64_t offset;
            if (!slot->handle || !slot->bo.allocation.BufferId || !base ||
                address < base || address - base >= allocation_bytes)
               continue;
            offset = address - base;
            if (offset > UINT32_MAX || allocation_bytes > UINT32_MAX)
               continue;
            bytes = stride * NVX_DRM_VERTEX_DUMP_VERTICES;
            if (bytes > NVX_DRM_VERTEX_DUMP_MAX_BYTES)
               bytes = NVX_DRM_VERTEX_DUMP_MAX_BYTES;
            if (bytes > allocation_bytes - offset)
               bytes = (uint32_t)(allocation_bytes - offset) & ~3U;
            if (!bytes)
               continue;
            memset(dump, 0, sizeof(*dump));
            dump->buffer_id = slot->bo.allocation.BufferId;
            dump->gpu_va = address;
            dump->allocation_bytes = (uint32_t)allocation_bytes;
            dump->offset = (uint32_t)offset;
            dump->bytes = bytes;
            dump->force_gpu_readback = force_gpu_readback;
            if (force_gpu_readback)
               device->shader_triangle_input_readback_scheduled = 1U;
            if (slot->bo.shadow && slot->bo.map_count &&
                !force_gpu_readback) {
               memcpy(dump->dwords, slot->bo.shadow + dump->offset, bytes);
               dump->copied_from_shadow = 1U;
            }
            return 1;
         }
         }
         goto next_address;
next_address:
         ;
      }
      cursor += count + 1U;
   }
   return 0;
}

/* Find only the BOs whose addresses are actually consumed by a known NVC0
 * address method. The submit helper needs one of the batch reference slots
 * for its own pushbuffer BO, so including every live allocation would fill
 * the reference array with unrelated buffers and make all later pushes fail. */
static int
nvx_drm_add_direct_address_reference(
   struct nvx_drm_device_slot *device, uint64_t address, uint32_t access,
   NVX_3D_BUFFER_REFERENCE *references, uint32_t *reference_count)
{
   uint32_t i, j;

   if (!address || !access || !references || !reference_count)
      return 1;
   for (i = 0; i < NVX_DRM_BUFFER_SLOTS; i++) {
      const struct nvx_drm_bo_slot *slot = &device->buffers[i];
      uint64_t start, bytes;
      if (!slot->handle || !slot->bo.allocation.BufferId ||
          slot->bo.allocation.ClientToken != device->client_token)
         continue;
      start = slot->bo.allocation.GpuVa;
      bytes = slot->bo.allocation.Bytes;
      if (!start || !bytes || address < start || address - start >= bytes)
         continue;
      for (j = 0; j < *reference_count; j++) {
         if (references[j].BufferId == slot->bo.allocation.BufferId) {
            references[j].Access |= access;
            return 1;
         }
      }
      /* Leave one slot for NvxWinsysSubmitBatch's staging pushbuffer BO. */
      if (*reference_count >= NVX_3D_MAX_BATCH_BUFFERS - 1U)
         return 0;
      references[*reference_count].BufferId =
         slot->bo.allocation.BufferId;
      references[*reference_count].Access = access;
      references[*reference_count].Reserved = 0;
      (*reference_count)++;
      return 1;
   }
   return 1;
}

static int
nvx_drm_add_nvc0_address_references(
   struct nvx_drm_device_slot *device, const uint8_t *dword_data,
   uint32_t dword_count, NVX_3D_BUFFER_REFERENCE *references,
   uint32_t *reference_count)
{
   uint32_t cursor = 0;

   while (cursor < dword_count) {
      uint32_t header, packet_type, count, method, subchannel, i;
      memcpy(&header, dword_data + (size_t)cursor * sizeof(uint32_t),
             sizeof(header));
      packet_type = header & 0xe0000000U;
      if (packet_type == 0x80000000U) {
         cursor++;
         continue;
      }
      if (packet_type != 0x20000000U && packet_type != 0x60000000U &&
          packet_type != 0xa0000000U)
         return 1; /* The miniport performs the authoritative rejection. */
      count = (header >> 16) & 0x1fffU;
      method = (header & 0x1fffU) << 2;
      subchannel = (header >> 13) & 7U;
      if (!count || count > dword_count - cursor - 1U)
         return 1;
      for (i = 0; i < count; i++) {
         uint32_t current_method, pair_index, array_index;
         int kind;
         if (packet_type == 0x20000000U)
            current_method = method + i * 4U;
         else if (packet_type == 0xa0000000U && i)
            current_method = method + 4U;
         else
            current_method = method;
         kind = Nvx3dNvc0AddressMethodKind(subchannel, current_method,
                                          &pair_index, &array_index);
         if (kind == 1) {
            const NVX_3D_NVC0_ADDRESS_PAIR *pair =
               &Nvx3dNvc0AddressPairs[pair_index];
            uint32_t high, low;
            uint64_t address;
            if (i + 1U >= count)
               return 1;
            memcpy(&high, dword_data + (size_t)(cursor + 1U + i) * 4U, 4U);
            memcpy(&low, dword_data + (size_t)(cursor + 2U + i) * 4U, 4U);
            address = ((uint64_t)high << 32) | low;
#if defined(_WIN32) || defined(_WIN64)
            if (pair->HighMethod >= 0x1c04U &&
                pair->HighMethod <= 0x1df4U &&
                pair->IndexStride == 0x10U &&
                current_method == pair->HighMethod +
                                  array_index * pair->IndexStride && i > 0U &&
                packet_type == 0x20000000U &&
                subchannel == 0U) {
               uint32_t fetch, stride, vertex_count, slot_index;
               uint64_t bytes;
               fetch = Nvx3dBatchReadDword(dword_data, cursor + i);
               stride = fetch & 0x0fffU;
               vertex_count = device->gr_state[0x0d78U / 4U];
               if ((fetch & 0x1000U) && stride && vertex_count &&
                   vertex_count <= UINT32_MAX / stride) {
                  bytes = (uint64_t)stride * vertex_count;
                  for (slot_index = 0; slot_index < NVX_DRM_BUFFER_SLOTS;
                       slot_index++) {
                     struct nvx_drm_bo_slot *slot =
                        &device->buffers[slot_index];
                     uint64_t base = slot->bo.allocation.GpuVa;
                     uint64_t offset;
                     if (!slot->handle || !slot->bo.allocation.BufferId ||
                         !base || address < base ||
                         address - base >= slot->bo.allocation.Bytes)
                        continue;
                     offset = address - base;
                     if (bytes > (uint64_t)slot->bo.allocation.Bytes - offset)
                        break;
                     if (slot->bo.map_count && slot->bo.shadow) {
                        uint32_t upload_ok = (uint32_t)
                           NvxNouveauBoFlushRange(&slot->bo, offset, bytes);
                        if (!upload_ok)
                           return 0;
                        if (nvx_drm_vertex_upload_trace_count < 32U) {
                           uint32_t trace_index =
                              nvx_drm_vertex_upload_trace_count++;
                           NvxWglRecordNouveauPushbufItem(16U, trace_index,
                              (uint32_t)slot->bo.allocation.BufferId,
                              (uint32_t)offset, (uint32_t)bytes,
                              slot->bo.map_count, slot->bo.map_access, 1U);
                        }
                        if (address == device->shader_cube_vertex_address)
                           device->shader_cube_vertex_flush_done = 1U;
#if defined(_WIN32) || defined(_WIN64)
                        if (address == device->shader_cube_vertex_address &&
                            !device->shader_cube_vertex_readback_scheduled &&
                            bytes <= NVX_DRM_VERTEX_DUMP_MAX_BYTES) {
                           uint32_t gpu_dwords[
                              NVX_DRM_VERTEX_DUMP_MAX_BYTES / sizeof(uint32_t)];
                           uint32_t dword_count = (uint32_t)(bytes / 4U);
                           uint32_t readback_ok, matches = 0U;
                           NvxWglRecordNouveauBufferDwords(
                              (uint32_t)slot->bo.allocation.BufferId,
                              address, (uint32_t)offset,
                              (const uint32_t *)(slot->bo.shadow +
                                                (size_t)offset),
                              dword_count, 1U);
                           readback_ok = (uint32_t)NvxWinsysDownloadBuffer(
                              &device->device, slot->bo.allocation.BufferId,
                              (uint32_t)slot->bo.allocation.Bytes,
                              (uint32_t)offset, gpu_dwords,
                              (uint32_t)bytes);
                           if (readback_ok) {
                              matches = (uint32_t)(memcmp(
                                 slot->bo.shadow + (size_t)offset,
                                 gpu_dwords, (size_t)bytes) == 0);
                              NvxWglRecordNouveauBufferDwords(
                                 (uint32_t)slot->bo.allocation.BufferId,
                                 address, (uint32_t)offset, gpu_dwords,
                                 dword_count, 0U);
                           }
                           if (nvx_drm_vertex_upload_trace_count < 32U) {
                              uint32_t trace_index =
                                 nvx_drm_vertex_upload_trace_count++;
                              NvxWglRecordNouveauPushbufItem(17U, trace_index,
                                 (uint32_t)slot->bo.allocation.BufferId,
                                 (uint32_t)offset, (uint32_t)bytes,
                                 matches, readback_ok, 0U);
                           }
                           device->shader_cube_vertex_readback_scheduled = 1U;
                        }
#endif
                     }
                     break;
                  }
               }
            }
#endif
            if (!nvx_drm_add_direct_address_reference(device, address,
                     pair->Access, references, reference_count))
               return 0;
            i++;
         }
      }
      cursor += count + 1U;
   }
   return 1;
}

/* Translate the bounded Nouveau subset into the NVX batch ABI. The miniport
 * validates ownership, packet methods and addresses before submitting it. */
static int
nvx_drm_submit_pushbuf(int fd,
                       struct drm_nouveau_gem_pushbuf *request)
{
   struct nvx_drm_device_slot *device;
   struct drm_nouveau_gem_pushbuf_bo *buffers;
   struct drm_nouveau_gem_pushbuf_push *pushes;
   NVX_WINSYS_DEVICE winsys;
   NVX_3D_BATCH_HEADER header;
   NVX_3D_BUFFER_REFERENCE references[NVX_3D_MAX_BATCH_BUFFERS];
   NVX_3D_RELOCATION translated_relocations[NVX_3D_MAX_BATCH_RELOCATIONS];
   uint8_t batch[NVX_3D_MAX_BATCH_BYTES];
   uint32_t dword_count = 0, reference_count = 0, relocation_count = 0, i;
   uint32_t flushed_shadow_count = 0;
   uint32_t reference_bytes, dword_bytes, relocation_bytes, batch_bytes;
   uint64_t client_token;
   uint64_t fence_value = 0;
   int valid = 0, translated = 0;
#if defined(_WIN32) || defined(_WIN64)
   struct nvx_drm_vertex_dump vertex_dumps[NVX_DRM_VERTEX_DUMP_LIMIT];
   uint32_t vertex_dump_count = 0;
   struct nvx_drm_shader_dump shader_dumps[NVX_DRM_SHADER_DUMP_LIMIT];
   uint32_t shader_dump_count = 0;
#endif

   if (!request || !request->nr_push ||
       request->nr_push > NOUVEAU_GEM_MAX_PUSH || !request->nr_buffers ||
       request->nr_buffers > NVX_3D_MAX_BATCH_BUFFERS ||
       request->nr_relocs > NVX_3D_MAX_BATCH_RELOCATIONS ||
       !request->buffers || !request->push ||
       (request->nr_relocs && !request->relocs))
      return 0;

   buffers = (struct drm_nouveau_gem_pushbuf_bo *)(uintptr_t)request->buffers;
   pushes = (struct drm_nouveau_gem_pushbuf_push *)(uintptr_t)request->push;
   memset(translated_relocations, 0, sizeof(translated_relocations));
   memset(references, 0, sizeof(references));
   nvx_drm_lock_acquire();
   device = nvx_drm_find_device_locked(fd);
   if (!device || !device->device.NouveauChannelHandle ||
       request->channel != device->device.NouveauChannelHandle)
      goto done;

   for (i = 0; i < request->nr_buffers; i++) {
      struct nvx_drm_bo_slot *slot =
         nvx_drm_find_bo_locked(device, buffers[i].handle);
      uint32_t access = 0, j;
      if (buffers[i].read_domains)
         access |= NVX_3D_RELOC_READ;
      if (buffers[i].write_domains)
         access |= NVX_3D_RELOC_WRITE;
      if (!slot || !access || !slot->bo.allocation.BufferId ||
          slot->bo.allocation.ClientToken != device->client_token)
         goto done;
      for (j = 0; j < reference_count; j++)
         if (references[j].BufferId == slot->bo.allocation.BufferId)
            break;
      if (j == reference_count) {
         if (reference_count >= NVX_3D_MAX_BATCH_BUFFERS)
            goto done;
         references[j].BufferId = slot->bo.allocation.BufferId;
         references[j].Access = access;
         references[j].Reserved = 0;
         reference_count++;
      } else {
         references[j].Access |= access;
      }
   }


   for (i = 0; i < request->nr_push; i++) {
      struct nvx_drm_bo_slot *slot;
      uint64_t bytes = pushes[i].length &
                        ~((uint64_t)NOUVEAU_GEM_PUSHBUF_NO_PREFETCH);
      uint32_t handle;
      if (pushes[i].bo_index >= request->nr_buffers || !bytes ||
          (bytes & 3U) || bytes > UINT32_MAX ||
          pushes[i].offset > UINT32_MAX)
         goto done;
      handle = buffers[pushes[i].bo_index].handle;
      slot = nvx_drm_find_bo_locked(device, handle);
      if (!slot || !slot->bo.shadow || !slot->bo.map_count ||
          pushes[i].offset > slot->bo.allocation.Bytes ||
          bytes > slot->bo.allocation.Bytes - pushes[i].offset ||
          bytes / sizeof(uint32_t) > NVX_3D_MAX_PUSH_DWORDS - dword_count)
         goto done;
      memcpy(batch + sizeof(header) +
             (size_t)NVX_3D_MAX_BATCH_BUFFERS * sizeof(references[0]) +
             (size_t)dword_count * sizeof(uint32_t),
             slot->bo.shadow + (uint32_t)pushes[i].offset, (size_t)bytes);
      dword_count += (uint32_t)(bytes / sizeof(uint32_t));
   }

#if defined(_WIN32) || defined(_WIN64)
   if (shader_dump_count < NVX_DRM_SHADER_DUMP_LIMIT) {
      shader_dump_count = nvx_drm_capture_shader_addresses_locked(device,
          batch + sizeof(header) +
             (size_t)NVX_3D_MAX_BATCH_BUFFERS * sizeof(references[0]),
          dword_count, shader_dumps);
   }
#endif

   /* NVC0 also emits direct GPU addresses in its method streams, outside
    * request->buffers. Add references for those address operands only. */
   if (!nvx_drm_add_nvc0_address_references(device,
          batch + sizeof(header) +
             (size_t)NVX_3D_MAX_BATCH_BUFFERS * sizeof(references[0]),
          dword_count, references, &reference_count))
      goto done;

   /* Flush after direct NVC0 addresses have been resolved. Nouveau vertex BOs
    * can be referenced only by VERTEX_ARRAY_START, so they are absent from
    * request->buffers and were missed by an earlier flush pass. */
   for (i = 0; i < reference_count; i++) {
      struct nvx_drm_bo_slot *slot = NULL;
      uint32_t slot_index;
      if (!(references[i].Access & NVX_3D_RELOC_READ))
         continue;
      for (slot_index = 0; slot_index < NVX_DRM_BUFFER_SLOTS; slot_index++) {
         if (device->buffers[slot_index].handle &&
             device->buffers[slot_index].bo.allocation.BufferId ==
                references[i].BufferId) {
            slot = &device->buffers[slot_index];
            break;
         }
      }
      if (!slot)
         goto done;
      if (device->shader_triangle_input_vertex_address &&
          !device->shader_triangle_input_flush_trace_done &&
          slot->bo.allocation.GpuVa <=
             device->shader_triangle_input_vertex_address &&
          device->shader_triangle_input_vertex_address -
             slot->bo.allocation.GpuVa < slot->bo.allocation.Bytes) {
         uint32_t flush_ok = 0U;
         uint32_t offset = (uint32_t)(
            device->shader_triangle_input_vertex_address -
            slot->bo.allocation.GpuVa);
#if defined(_WIN32) || defined(_WIN64)
         if (slot->bo.shadow && offset <= slot->bo.allocation.Bytes &&
             12U * sizeof(uint32_t) <= slot->bo.allocation.Bytes - offset)
            NvxWglRecordNouveauBufferDwords(
               (uint32_t)references[i].BufferId,
               device->shader_triangle_input_vertex_address, offset,
               (const uint32_t *)(slot->bo.shadow + offset), 12U, 1U);
#endif
         /* This probe targets the immediate triangle's CPU-fed vertex range.
          * Its shadow can be written through Mesa's cached map without a
          * second map call carrying NOUVEAU_BO_WR. Ensure that exact source
          * BO is uploaded before the GPU consumes its direct address. */
         if (slot->bo.map_count && slot->bo.shadow) {
            slot->bo.map_access |= NVX_NOUVEAU_BO_WRITE;
            flush_ok = (uint32_t)NvxNouveauBoFlush(&slot->bo);
         }
         NvxWglRecordNouveauPushbufItem(14U, 0U,
            (uint32_t)references[i].BufferId,
            (uint32_t)(device->shader_triangle_input_vertex_address -
                      slot->bo.allocation.GpuVa),
            references[i].Access, slot->bo.map_count,
            slot->bo.map_access, flush_ok);
         device->shader_triangle_input_flush_trace_done = 1U;
         if (!flush_ok && slot->bo.map_count &&
             (slot->bo.map_access & NVX_NOUVEAU_BO_WRITE))
            goto done;
         if (flush_ok) {
            flushed_shadow_count++;
#if defined(_WIN32) || defined(_WIN64)
            if (!device->shader_triangle_input_readback_scheduled &&
                vertex_dump_count < NVX_DRM_VERTEX_DUMP_LIMIT &&
                offset <= slot->bo.allocation.Bytes &&
                12U * sizeof(uint32_t) <=
                   slot->bo.allocation.Bytes - offset) {
               struct nvx_drm_vertex_dump *dump =
                  &vertex_dumps[vertex_dump_count++];
               memset(dump, 0, sizeof(*dump));
               dump->buffer_id = slot->bo.allocation.BufferId;
               dump->gpu_va = device->shader_triangle_input_vertex_address;
               dump->allocation_bytes = (uint32_t)slot->bo.allocation.Bytes;
               dump->offset = offset;
               dump->bytes = 12U * sizeof(uint32_t);
               dump->force_gpu_readback = 1U;
               device->shader_triangle_input_readback_scheduled = 1U;
            }
#endif
         }
         continue;
      }
      if (!slot->bo.map_count ||
          !(slot->bo.map_access & NVX_NOUVEAU_BO_WRITE))
         continue;
      if (!NvxNouveauBoFlush(&slot->bo))
         goto done;
      flushed_shadow_count++;
      if (nvx_drm_shadow_flush_trace_count < 16U) {
         uint32_t trace_index = nvx_drm_shadow_flush_trace_count++;
         NvxWglRecordNouveauPushbufItem(13U, trace_index,
            (uint32_t)references[i].BufferId,
            (uint32_t)slot->bo.allocation.Bytes, references[i].Access,
            slot->bo.map_count, slot->bo.map_access, 1U);
      }
   }
#if defined(_WIN32) || defined(_WIN64)
   while (nvx_drm_vertex_dump_count < NVX_DRM_VERTEX_DUMP_LIMIT &&
          vertex_dump_count < NVX_DRM_VERTEX_DUMP_LIMIT) {
      struct nvx_drm_vertex_dump *dump =
         &vertex_dumps[vertex_dump_count];
      if (!nvx_drm_capture_vertex_shadow_locked(device,
             batch + sizeof(header) +
                (size_t)NVX_3D_MAX_BATCH_BUFFERS * sizeof(references[0]),
             dword_count, nvx_drm_vertex_dump_addresses,
             nvx_drm_vertex_dump_count, dump))
         break;
      nvx_drm_vertex_dump_addresses[nvx_drm_vertex_dump_count++] =
         dump->gpu_va;
      vertex_dump_count++;
   }
#endif

   /* Translate Nouveau's source-BO relocation into a relocation against an
    * owned NVX allocation. Preserve the GPU address in the copied batch and
    * reject OR/domain relocations until allocation domains are represented in
    * the NVX ABI. */
   for (i = 0; i < request->nr_relocs; i++) {
      const struct drm_nouveau_gem_pushbuf_reloc *reloc =
         (const struct drm_nouveau_gem_pushbuf_reloc *)(uintptr_t)request->relocs + i;
      struct nvx_drm_bo_slot *target_slot;
      struct nvx_drm_bo_slot *source_slot;
      uint64_t address;
      uint32_t push_index, command_word = 0, preceding_words = 0;
      uint32_t access = 0, flags = reloc->flags;
      uint32_t *source_dwords;
      int source_found = 0;

      if (reloc->bo_index >= request->nr_buffers ||
          reloc->reloc_bo_index >= request->nr_buffers ||
          (flags != NOUVEAU_GEM_RELOC_LOW &&
           flags != NOUVEAU_GEM_RELOC_HIGH))
         goto done;
      target_slot = nvx_drm_find_bo_locked(device,
                            buffers[reloc->bo_index].handle);
      source_slot = nvx_drm_find_bo_locked(device,
                            buffers[reloc->reloc_bo_index].handle);
      if (!target_slot || !source_slot || !source_slot->bo.shadow ||
          !target_slot->bo.allocation.BufferId ||
          target_slot->bo.allocation.ClientToken != device->client_token ||
          reloc->data >= target_slot->bo.allocation.Bytes ||
          (uint64_t)reloc->reloc_bo_offset + sizeof(uint32_t) >
             source_slot->bo.allocation.Bytes || (reloc->reloc_bo_offset & 3U))
         goto done;
      if (buffers[reloc->bo_index].read_domains)
         access |= NVX_3D_RELOC_READ;
      if (buffers[reloc->bo_index].write_domains)
         access |= NVX_3D_RELOC_WRITE;
      if (!access || target_slot->bo.allocation.GpuVa >
                     UINT64_MAX - reloc->data)
         goto done;
      address = target_slot->bo.allocation.GpuVa + reloc->data;

      for (push_index = 0; push_index < request->nr_push; push_index++) {
         uint64_t push_bytes = pushes[push_index].length &
                               ~((uint64_t)NOUVEAU_GEM_PUSHBUF_NO_PREFETCH);
         if (pushes[push_index].bo_index == reloc->reloc_bo_index &&
             reloc->reloc_bo_offset >= pushes[push_index].offset &&
             (uint64_t)reloc->reloc_bo_offset + sizeof(uint32_t) <=
                pushes[push_index].offset + push_bytes) {
            command_word = preceding_words + (uint32_t)(
               (reloc->reloc_bo_offset - pushes[push_index].offset) / 4U);
            source_found = 1;
            break;
         }
         if (!push_bytes || (push_bytes & 3U) ||
             push_bytes / 4U > NVX_3D_MAX_PUSH_DWORDS - preceding_words)
            goto done;
         preceding_words += (uint32_t)(push_bytes / 4U);
      }
      if (!source_found || command_word >= dword_count ||
          (flags == NOUVEAU_GEM_RELOC_HIGH && (address >> 32) > 0xffU))
         goto done;
      source_dwords = (uint32_t *)(void *)(batch + sizeof(header) +
         (size_t)NVX_3D_MAX_BATCH_BUFFERS * sizeof(references[0]));
      source_dwords[command_word] = flags == NOUVEAU_GEM_RELOC_LOW ?
                                     (uint32_t)address :
                                     (uint32_t)(address >> 32);
      translated_relocations[relocation_count].CommandWord = command_word;
      translated_relocations[relocation_count].BufferId =
                                      target_slot->bo.allocation.BufferId;
      translated_relocations[relocation_count].BufferOffset = reloc->data;
      translated_relocations[relocation_count].Bytes =
         target_slot->bo.allocation.Bytes - reloc->data;
      translated_relocations[relocation_count].Access = access;
      relocation_count++;
   }

   winsys = device->device;
   client_token = device->client_token;
   nvx_drm_lock_release();

#if defined(_WIN32) || defined(_WIN64)
   {
      uint32_t dump_index;
      for (dump_index = 0; dump_index < vertex_dump_count; dump_index++) {
         struct nvx_drm_vertex_dump *dump = &vertex_dumps[dump_index];
         uint32_t result = dump->copied_from_shadow ? 1U : 0U;
         if (dump->force_gpu_readback)
            continue;
         if (!dump->copied_from_shadow &&
             !NvxWinsysDownloadBuffer(&winsys, dump->buffer_id,
                                     dump->allocation_bytes, dump->offset,
                                     dump->dwords, dump->bytes))
            result = 2U;
         NvxWglRecordNouveauBufferDwords((uint32_t)dump->buffer_id,
            dump->gpu_va, dump->offset, dump->dwords,
            dump->bytes / sizeof(uint32_t), result);
      }
   }
#endif

   memset(&header, 0, sizeof(header));
   header.Size = sizeof(header);
   header.Version = NVX_3D_ABI_VERSION;
   header.DwordCount = dword_count;
   header.BufferReferenceCount = reference_count;
   header.RelocationCount = relocation_count;
   header.ClientToken = client_token;
   reference_bytes = reference_count * (uint32_t)sizeof(references[0]);
   dword_bytes = dword_count * (uint32_t)sizeof(uint32_t);
   relocation_bytes = relocation_count * (uint32_t)sizeof(translated_relocations[0]);
   batch_bytes = (uint32_t)sizeof(header) + reference_bytes + dword_bytes +
                 relocation_bytes;
   memmove(batch + sizeof(header) + reference_bytes,
           batch + sizeof(header) +
              (size_t)NVX_3D_MAX_BATCH_BUFFERS * sizeof(references[0]),
           dword_bytes);
   memcpy(batch, &header, sizeof(header));
   memcpy(batch + sizeof(header), references, reference_bytes);
   translated = Nvx3dTranslateNvc0ObjectBindings(
      batch + sizeof(header) + reference_bytes, dword_count,
      winsys.NouveauObjectBindings, winsys.NouveauObjectBindingCount);
   if (relocation_bytes)
      memcpy(batch + sizeof(header) + reference_bytes + dword_bytes,
             translated_relocations, relocation_bytes);
   if (translated) {
      valid = NvxWinsysSubmitBatch(&winsys, batch, batch_bytes,
                                   &fence_value);
   }
   if (valid && fence_value) {
      uint32_t reference_index;
      nvx_drm_lock_acquire();
      device = nvx_drm_find_device_locked(fd);
      if (device) {
         for (reference_index = 0; reference_index < reference_count;
              reference_index++) {
            unsigned slot_index;
            for (slot_index = 0; slot_index < NVX_DRM_BUFFER_SLOTS;
                 slot_index++) {
               struct nvx_drm_bo_slot *slot = &device->buffers[slot_index];
               if (slot->handle && slot->bo.allocation.BufferId ==
                                      references[reference_index].BufferId) {
                  int marked = NvxNouveauBoMarkUsed(&slot->bo, fence_value);
                  /* Mesa keeps GART BOs mapped through persistent shadows.
                   * Keep readable mappings current after GPU writes so a
                   * later READ map sees completed render output rather than
                   * stale CPU cache contents. */
                  if ((references[reference_index].Access &
                       NVX_3D_RELOC_WRITE) && slot->bo.map_count &&
                      (slot->bo.map_access & NVX_NOUVEAU_BO_READ))
                     (void)NvxNouveauBoRefreshRead(&slot->bo);
                  if (nvx_drm_fence_trace_count < 4U) {
                     uint32_t trace_index = nvx_drm_fence_trace_count++;
                     NvxWglRecordNouveauPushbufItem(9U, trace_index,
                        (uint32_t)slot->bo.allocation.BufferId,
                        (uint32_t)fence_value, (uint32_t)marked,
                        (uint32_t)slot->bo.last_fence, 0U, 0U);
                  }
                  break;
               }
            }
         }
      }
      nvx_drm_lock_release();
   }
#if defined(_WIN32) || defined(_WIN64)
   if (valid && fence_value) {
      uint32_t dump_index;
      int has_forced_dump = 0;
      int completed = 0;
      for (dump_index = 0; dump_index < vertex_dump_count; dump_index++)
         if (vertex_dumps[dump_index].force_gpu_readback)
            has_forced_dump = 1;
      if (has_forced_dump)
         completed = NvxWinsysWaitFence(&winsys, fence_value, 5000U);
      for (dump_index = 0; dump_index < vertex_dump_count; dump_index++) {
         struct nvx_drm_vertex_dump *dump = &vertex_dumps[dump_index];
         uint32_t result;
         if (!dump->force_gpu_readback)
            continue;
         result = completed ? 0U : 3U;
         if (completed && !NvxWinsysDownloadBuffer(&winsys, dump->buffer_id,
               dump->allocation_bytes, dump->offset, dump->dwords,
               dump->bytes))
            result = 2U;
         NvxWglRecordNouveauBufferDwords((uint32_t)dump->buffer_id,
            dump->gpu_va, dump->offset, dump->dwords,
            dump->bytes / sizeof(uint32_t), result);
      }
   }
   if (valid && fence_value && shader_dump_count) {
      uint32_t dump_index;
      int completed = NvxWinsysWaitFence(&winsys, fence_value, 5000U);
      for (dump_index = 0; dump_index < shader_dump_count; dump_index++) {
         struct nvx_drm_shader_dump *dump = &shader_dumps[dump_index];
         uint32_t result = 3U;
         if (completed) {
            result = NvxWinsysDownloadBuffer(&winsys, dump->buffer_id,
               dump->allocation_bytes, dump->offset, dump->dwords,
               dump->bytes) ? 0U : 2U;
         }
         NvxWglRecordNouveauShaderDwords(dump->stage,
            (uint32_t)dump->buffer_id, dump->gpu_va, dump->offset,
            dump->dwords, dump->bytes / sizeof(uint32_t),
            completed && result == 0U ? 4U : result, dump->draw_kind);
      }
   }
#endif
   if (valid) {
      /* nouveau_pushbuf_flush() uses these kernel outputs to establish the
       * per-domain allocation budget. Leaving them as the caller's zeroed
       * input makes pushbuf_kref_fits() reject every subsequent BO with
       * "PBK: memory domain limit", even though GEM_NEW allocated it. Keep
       * the response consistent with the empty pushbuf initialization ioctl. */
      request->suffix0 = 0;
      request->suffix1 = 0;
      request->vram_available = winsys.caps.VramApertureBytes;
      request->gart_available = winsys.caps.GartBytes;
   }
   NvxWglRecordNouveauPushbufItem(6U, 0U,
                                  !translated ? 3U : (valid ? 1U : 2U),
                                  dword_count, reference_count,
                                  request->nr_relocs, relocation_count,
                                  flushed_shadow_count);
   return valid;

done:
   nvx_drm_lock_release();
   NvxWglRecordNouveauPushbufItem(6U, 0U, 0U,
                                  dword_count, reference_count,
                                  request->nr_relocs, 0U,
                                  flushed_shadow_count);
   return 0;
}
#endif

static struct nvx_drm_device_slot *
nvx_drm_find_device_locked(int fd)
{
   unsigned i;
   for (i = 0; i < NVX_DRM_DEVICE_SLOTS; i++)
      if (nvx_drm_devices[i].fd == fd)
         return &nvx_drm_devices[i];
   return NULL;
}

static struct nvx_drm_bo_slot *
nvx_drm_find_bo_locked(struct nvx_drm_device_slot *device, uint32_t handle)
{
   unsigned i;
   if (!device || !handle)
      return NULL;
   for (i = 0; i < NVX_DRM_BUFFER_SLOTS; i++)
      if (device->buffers[i].handle == handle)
         return &device->buffers[i];
   return NULL;
}

/* nouveau_object_channel_new stores the signed drm_nouveau_channel_alloc
 * channel field in a 64-bit object handle. RM handles such as C56Fxxxx have
 * bit 31 set and therefore arrive in NVIF tokens sign-extended. */
static int
nvx_drm_channel_token_matches(uint64_t token, uint32_t channel)
{
   uint32_t high = (uint32_t)(token >> 32);

   return (uint32_t)token == channel &&
          (high == 0U || ((channel & 0x80000000U) && high == UINT32_MAX));
}

static int
nvx_drm_getparam(int fd, void *data, unsigned long size)
{
   struct drm_nouveau_getparam *request = data;
   NVX_WINSYS_DEVICE device;

   if (!request || size != sizeof(*request) ||
       !nvx_drm_lookup(fd, &device))
      return nvx_drm_unsupported();
   switch (request->param) {
   case NOUVEAU_GETPARAM_FB_SIZE:
      /* Report available VRAM heap size (~32 MiB) rather than the entire
       * 128 MiB aperture so Mesa accurately paces VRAM consumption and
       * prefers GART for large allocations like TLS. */
      request->value = (device.caps.VramApertureBytes > 0x02000000ULL) ?
                       0x02000000ULL : device.caps.VramApertureBytes;
      break;
   case NOUVEAU_GETPARAM_AGP_SIZE:
      request->value = device.caps.GartBytes;
      break;
   case NOUVEAU_GETPARAM_PCI_VENDOR:
      request->value = 0x10deU;
      break;
   case NOUVEAU_GETPARAM_PCI_DEVICE:
      request->value = device.caps.DeviceId;
      break;
   case NOUVEAU_GETPARAM_CHIPSET_ID:
      request->value = device.caps.Chipset;
      break;
   case NOUVEAU_GETPARAM_EXEC_PUSH_MAX:
      request->value = NOUVEAU_GEM_MAX_PUSH;
      break;
   case NOUVEAU_GETPARAM_GRAPH_UNITS:
      /* GA104 RTX 3070 (PCI 2488): 6 GPCs, 46 enabled TPCs. */
      if (device.caps.DeviceId != 0x2488U || device.caps.Chipset != 0x170U)
         return nvx_drm_unsupported_operation(
            NVX_XF86DRM_UNSUPPORTED_WRITE_READ, DRM_NOUVEAU_GETPARAM);
      request->value = 6U | (46U << 8);
      break;
   default:
#if defined(_WIN32) || defined(_WIN64)
      NvxWglRecordNouveauGetparam(request->param, 0, 1U);
#endif
      return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_WRITE_READ,
                                          DRM_NOUVEAU_GETPARAM);
   }
#if defined(_WIN32) || defined(_WIN64)
   NvxWglRecordNouveauGetparam(request->param, (uint32_t)request->value, 0U);
#endif
   return 0;
}

static int
nvx_drm_nvif_device_new(int fd, void *data, unsigned long size)
{
   struct nvif_ioctl_v0 *ioctl = data;
   struct nvif_ioctl_new_v0 *create;
   NVX_WINSYS_DEVICE device;
   const unsigned long minimum = sizeof(*ioctl) + sizeof(*create) +
                                 sizeof(struct nv_device_v0);

   if (!nvx_drm_lookup(fd, &device) || !data ||
       size != minimum || ioctl->type != NVIF_IOCTL_V0_NEW)
      return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_WRITE,
                                          DRM_NOUVEAU_NVIF);
   create = (struct nvif_ioctl_new_v0 *)((uint8_t *)data + sizeof(*ioctl));
   if (create->oclass != NV_DEVICE || create->route != NVIF_IOCTL_V0_ROUTE_NVIF)
      return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_WRITE,
                                          DRM_NOUVEAU_NVIF);
   /* nouveau_device_new() creates a client-side descriptor. NVX owns the
    * actual channel and engine objects, so this does not bind a GPU object. */
   return 0;
}

static int
nvx_drm_nvif_device_info(int fd, void *data, unsigned long size)
{
   struct nvif_ioctl_v0 *ioctl = data;
   struct nvif_ioctl_mthd_v0 *method;
   struct nv_device_info_v0 *info;
   NVX_WINSYS_DEVICE device;
   const unsigned long expected = sizeof(*ioctl) + sizeof(*method) +
                                  sizeof(*info);

   if (!data || size != expected || !nvx_drm_lookup(fd, &device) ||
       ioctl->type != NVIF_IOCTL_V0_MTHD)
      return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_WRITE_READ,
                                          DRM_NOUVEAU_NVIF);
   method = (struct nvif_ioctl_mthd_v0 *)((uint8_t *)data + sizeof(*ioctl));
   if (method->method != NV_DEVICE_V0_INFO)
      return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_WRITE_READ,
                                          DRM_NOUVEAU_NVIF);
   info = (struct nv_device_info_v0 *)((uint8_t *)method + sizeof(*method));
   memset(info, 0, sizeof(*info));
   info->version = 0;
   info->platform = NV_DEVICE_INFO_V0_PCIE;
   info->chipset = (uint16_t)device.caps.Chipset;
   info->ram_size = device.caps.VramApertureBytes;
   memcpy(info->chip, "GA104", sizeof("GA104"));
   memcpy(info->name, "NVX Ampere device", sizeof("NVX Ampere device"));
   return 0;
}

static int
nvx_drm_nvif_sclass(int fd, void *data, unsigned long size)
{
   struct nvif_ioctl_v0 *ioctl = data;
   struct nvif_ioctl_sclass_v0 *sclass;
   struct nvx_drm_device_slot *device;
   unsigned long prefix = sizeof(*ioctl) + sizeof(*sclass);
   unsigned long expected;
   uint8_t capacity;
   uint32_t classes[16];
   unsigned class_count = 0;
   unsigned i;
   int found = 0;

   if (!data || size < prefix || ioctl->version != 0 ||
       ioctl->type != NVIF_IOCTL_V0_SCLASS)
      return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_WRITE_READ,
                                           DRM_NOUVEAU_NVIF);
   sclass = (struct nvif_ioctl_sclass_v0 *)((uint8_t *)data + sizeof(*ioctl));
   capacity = sclass->count;
   expected = prefix + (unsigned long)capacity *
      sizeof(struct nvif_ioctl_sclass_oclass_v0);
   if (capacity > NVX_DRM_MAX_SCLASS || size != expected ||
       sclass->version != 0 ||
       ioctl->route != NVIF_IOCTL_V0_ROUTE_HIDDEN)
      return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_WRITE_READ,
                                           DRM_NOUVEAU_NVIF);

   nvx_drm_lock_acquire();
   device = nvx_drm_find_device_locked(fd);
   nvx_drm_status.SclassCalls++;
   nvx_drm_status.LastSclassResult = 1U;
   nvx_drm_status.LastSclassCount = 0;
   memset(nvx_drm_status.LastSclass, 0,
          sizeof(nvx_drm_status.LastSclass));
   if (device && device->device.NouveauChannelHandle &&
       nvx_drm_channel_token_matches(
          ioctl->token, device->device.NouveauChannelHandle) &&
       device->device.NouveauGraphicsObjectHandle) {
      found = 1;
      if (device->device.caps.Features & NVX_3D_FEATURE_NVC0_M2MF_OBJECT)
         classes[class_count++] = NVX_MESA_CLASS_NVE4_P2MF;
      if (device->device.caps.Features & NVX_3D_FEATURE_NVC0_COPY_OBJECT)
         classes[class_count++] = NVX_RM_CLASS_AMPERE_DMA_COPY_B;
      if (device->device.caps.Features & NVX_3D_FEATURE_NVC0_2D_OBJECT)
         classes[class_count++] = NVX_RM_CLASS_FERMI_2D_A;
      if (device->device.NouveauComputeObjectHandle)
         classes[class_count++] = NVX_RM_CLASS_AMPERE_COMPUTE_B;
      classes[class_count++] = device->device.caps.GraphicsClass;
      nvx_drm_status.LastSclassResult = 0;
      nvx_drm_status.LastSclassCount = class_count;
      for (i = 0; i < class_count; i++)
         nvx_drm_status.LastSclass[i] = classes[i];
   }
   if (found && capacity) {
      struct nvif_ioctl_sclass_oclass_v0 *entry =
         (struct nvif_ioctl_sclass_oclass_v0 *)((uint8_t *)sclass +
                                                 sizeof(*sclass));
      unsigned copied = capacity < class_count ? capacity : class_count;
      for (i = 0; i < copied; i++) {
         entry[i].oclass = (int32_t)classes[i];
         entry[i].minver = 0;
         entry[i].maxver = 0;
      }
   }
   nvx_drm_lock_release();

#if defined(_WIN32) || defined(_WIN64)
   NvxWglRecordNouveauSclass((uint32_t)ioctl->token,
                             (uint32_t)(ioctl->token >> 32),
                             found ? 0U : 1U, class_count, classes);
#endif

   if (!found) {
      errno = ENODEV;
      return -1;
   }
   sclass->count = (uint8_t)class_count;
   return 0;
}

static uint32_t
nvx_drm_rm_object_for_class(const NVX_WINSYS_DEVICE *device, uint32_t oclass,
                            uint32_t *rm_class)
{
   if (!device || !rm_class)
      return 0;
   if (oclass == device->caps.GraphicsClass &&
       device->NouveauGraphicsObjectHandle) {
      *rm_class = oclass;
      return device->NouveauGraphicsObjectHandle;
   }
   if (oclass == NVX_RM_CLASS_AMPERE_COMPUTE_B &&
       device->NouveauComputeObjectHandle) {
      *rm_class = oclass;
      return device->NouveauComputeObjectHandle;
   }
   if (oclass == NVX_MESA_CLASS_NVE4_P2MF &&
       (device->caps.Features & NVX_3D_FEATURE_NVC0_M2MF_OBJECT)) {
      *rm_class = NVX_RM_CLASS_KEPLER_I2M_B;
      return NVX_RM_OBJECT_NVC0_M2MF;
   }
   if (oclass == NVX_RM_CLASS_AMPERE_DMA_COPY_B &&
       (device->caps.Features & NVX_3D_FEATURE_NVC0_COPY_OBJECT)) {
      *rm_class = oclass;
      return NVX_RM_OBJECT_NVC0_COPY;
   }
   if (oclass == NVX_RM_CLASS_FERMI_2D_A &&
       (device->caps.Features & NVX_3D_FEATURE_NVC0_2D_OBJECT)) {
      *rm_class = oclass;
      return NVX_RM_OBJECT_NVC0_2D;
   }
   return 0;
}

static int
nvx_drm_nvif_channel_object_new(int fd, void *data, unsigned long size)
{
   struct nvif_ioctl_v0 *ioctl = data;
   struct nvif_ioctl_new_v0 *create;
   struct nvx_drm_device_slot *device;
   struct nvx_drm_object_slot *slot = NULL;
   uint32_t rm_class = 0, rm_handle;
   unsigned i;
   const unsigned long expected = sizeof(*ioctl) + sizeof(*create);

   if (!data || size != expected || ioctl->version != 0 ||
       ioctl->type != NVIF_IOCTL_V0_NEW ||
       ioctl->route != NVIF_IOCTL_V0_ROUTE_HIDDEN)
      return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_WRITE,
                                           DRM_NOUVEAU_NVIF);
   create = (struct nvif_ioctl_new_v0 *)((uint8_t *)data + sizeof(*ioctl));
   if (create->version != 0 || create->route != NVIF_IOCTL_V0_ROUTE_NVIF ||
       !create->object)
      return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_WRITE,
                                           DRM_NOUVEAU_NVIF);

#if defined(_WIN32) || defined(_WIN64)
   NvxWglRecordNouveauObject((uint32_t)ioctl->token,
                             (uint32_t)(ioctl->token >> 32),
                             (uint32_t)create->oclass,
                             (uint32_t)create->handle, 0, 0, 1U);
#endif

   nvx_drm_lock_acquire();
   device = nvx_drm_find_device_locked(fd);
   if (!device || !nvx_drm_channel_token_matches(
                     ioctl->token, device->device.NouveauChannelHandle)) {
      nvx_drm_lock_release();
      errno = device ? EINVAL : ENODEV;
      return -1;
   }
   rm_handle = nvx_drm_rm_object_for_class(&device->device,
                                           (uint32_t)create->oclass,
                                           &rm_class);
   if (rm_handle) {
      for (i = 0; i < NVX_DRM_OBJECT_SLOTS; i++) {
         if (device->objects[i].user_object == create->object) {
            nvx_drm_lock_release();
            errno = EEXIST;
            return -1;
         }
         if (!device->objects[i].user_object && !slot)
            slot = &device->objects[i];
      }
   }
   if (!rm_handle || !slot) {
      nvx_drm_lock_release();
      errno = rm_handle ? ENOSPC : ENODEV;
      return -1;
   }
   slot->user_object = create->object;
   slot->user_handle = create->handle;
   slot->user_class = (uint32_t)create->oclass;
   slot->rm_handle = rm_handle;
   slot->rm_class = rm_class;
   nvx_drm_lock_release();
#if defined(_WIN32) || defined(_WIN64)
   NvxWglRecordNouveauObject((uint32_t)ioctl->token,
                             (uint32_t)(ioctl->token >> 32),
                             (uint32_t)create->oclass,
                             (uint32_t)create->handle, rm_class, rm_handle,
                             0U);
#endif
   return 0;
}

static int
nvx_drm_nvif_write(int fd, void *data, unsigned long size)
{
   struct nvif_ioctl_v0 *ioctl = data;

   if (!data || size < sizeof(*ioctl))
      return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_WRITE,
                                           DRM_NOUVEAU_NVIF);
   if (ioctl->type == NVIF_IOCTL_V0_NEW) {
      struct nvif_ioctl_new_v0 *create;
      if (size < sizeof(*ioctl) + sizeof(*create))
         return nvx_drm_unsupported_operation(
            NVX_XF86DRM_UNSUPPORTED_WRITE, DRM_NOUVEAU_NVIF);
      create = (struct nvif_ioctl_new_v0 *)((uint8_t *)data + sizeof(*ioctl));
      if (create->oclass == NV_DEVICE)
         return nvx_drm_nvif_device_new(fd, data, size);
      return nvx_drm_nvif_channel_object_new(fd, data, size);
   }
   if (ioctl->type == NVIF_IOCTL_V0_DEL)
      return nvx_drm_nvif_object_del(fd, data, size);
   return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_WRITE,
                                        DRM_NOUVEAU_NVIF);
}

static int
nvx_drm_nvif_object_del(int fd, void *data, unsigned long size)
{
   struct nvif_ioctl_v0 *ioctl = data;
   struct nvx_drm_device_slot *device;
   unsigned i;

   if (!data || size != sizeof(*ioctl) || ioctl->version != 0 ||
       ioctl->type != NVIF_IOCTL_V0_DEL || !ioctl->object)
      return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_WRITE,
                                           DRM_NOUVEAU_NVIF);
   nvx_drm_lock_acquire();
   device = nvx_drm_find_device_locked(fd);
   if (device) {
      for (i = 0; i < NVX_DRM_OBJECT_SLOTS; i++) {
         if (device->objects[i].user_object == ioctl->object) {
            memset(&device->objects[i], 0, sizeof(device->objects[i]));
            nvx_drm_lock_release();
            return 0;
         }
      }
   }
   nvx_drm_lock_release();
   errno = device ? ENOENT : ENODEV;
   return -1;
}

static int
nvx_drm_channel_alloc(int fd, struct drm_nouveau_channel_alloc *request,
                      unsigned long size)
{
   struct nvx_drm_device_slot *device;
   int ok = 0;

   if (!request || size != sizeof(*request) || request->channel ||
       request->pushbuf_domains || request->notifier_handle ||
       request->nr_subchan || request->fb_ctxdma_handle != 0xffffffffU ||
       request->tt_ctxdma_handle != NOUVEAU_FIFO_ENGINE_GR) {
      nvx_drm_status.ChannelAllocCount++;
      nvx_drm_status.LastChannelAllocResult = 1U;
      nvx_drm_status.LastChannelHandle = 0;
      return nvx_drm_unsupported_operation(
         NVX_XF86DRM_UNSUPPORTED_WRITE_READ, DRM_NOUVEAU_CHANNEL_ALLOC);
   }

   nvx_drm_lock_acquire();
   device = nvx_drm_find_device_locked(fd);
   nvx_drm_status.ChannelAllocCount++;
   if (device && NvxWinsysOpenNouveauChannel(&device->device)) {
      request->channel = (int32_t)device->device.NouveauChannelHandle;
      request->pushbuf_domains = NOUVEAU_GEM_DOMAIN_VRAM |
                                 NOUVEAU_GEM_DOMAIN_GART;
      request->notifier_handle = 0;
      nvx_drm_status.LastChannelAllocResult = 0;
      nvx_drm_status.LastChannelHandle =
         device->device.NouveauChannelHandle;
      ok = 1;
   } else {
      nvx_drm_status.LastChannelAllocResult = device ? 2U : 3U;
      nvx_drm_status.LastChannelHandle = 0;
   }
   nvx_drm_lock_release();
   if (ok)
      return 0;
   errno = device ? EBUSY : ENODEV;
   return -1;
}

static int
nvx_drm_channel_free(int fd, struct drm_nouveau_channel_free *request,
                     unsigned long size)
{
   struct nvx_drm_device_slot *device;
   int ok = 0;

   if (!request || size != sizeof(*request) || request->channel == 0)
      return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_WRITE,
                                           DRM_NOUVEAU_CHANNEL_FREE);
   nvx_drm_lock_acquire();
   device = nvx_drm_find_device_locked(fd);
   if (device &&
       (uint32_t)request->channel == device->device.NouveauChannelHandle)
      ok = NvxWinsysCloseNouveauChannel(&device->device);
   nvx_drm_lock_release();
   if (ok)
      return 0;
   errno = device ? EINVAL : ENODEV;
   return -1;
}

/* Mesa issues one empty pushbuf request during nouveau_pushbuf_new() to
 * initialize its suffix state. It contains no command stream and needs no GPU
 * submission. Keep this separate from real GEM_PUSHBUF kicks. */
static int
nvx_drm_pushbuf_nop(int fd, struct drm_nouveau_gem_pushbuf *request,
                    unsigned long size)
{
   struct nvx_drm_device_slot *device;

   if (!request || size != sizeof(*request))
      return nvx_drm_unsupported_operation(
         NVX_XF86DRM_UNSUPPORTED_WRITE_READ, DRM_NOUVEAU_GEM_PUSHBUF);

   if (request->nr_push || request->nr_buffers || request->nr_relocs ||
       request->buffers || request->relocs || request->push ||
       request->suffix0 || request->suffix1)
#if defined(_WIN32) || defined(_WIN64)
   {
      const struct drm_nouveau_gem_pushbuf_bo *buffers =
         (const struct drm_nouveau_gem_pushbuf_bo *)(uintptr_t)request->buffers;
      const struct drm_nouveau_gem_pushbuf_push *pushes =
         (const struct drm_nouveau_gem_pushbuf_push *)(uintptr_t)request->push;
      const struct drm_nouveau_gem_pushbuf_reloc *relocs =
         (const struct drm_nouveau_gem_pushbuf_reloc *)(uintptr_t)request->relocs;
      uint32_t i;

      if (buffers && request->nr_buffers <= NOUVEAU_GEM_MAX_BUFFERS) {
         for (i = 0; i < request->nr_buffers && i < 16U; i++) {
            NvxWglRecordNouveauPushbufItem(1U, i, buffers[i].handle,
               buffers[i].read_domains, buffers[i].write_domains,
               buffers[i].valid_domains, buffers[i].presumed.valid,
               buffers[i].presumed.domain);
         }
      }
      if (pushes && request->nr_push <= NOUVEAU_GEM_MAX_PUSH) {
         for (i = 0; i < request->nr_push && i < 16U; i++) {
            NvxWglRecordNouveauPushbufItem(2U, i, pushes[i].bo_index,
               (uint32_t)pushes[i].offset, (uint32_t)(pushes[i].offset >> 32),
               (uint32_t)pushes[i].length,
               (uint32_t)(pushes[i].length >> 32), pushes[i].pad);
            if (buffers && pushes[i].bo_index < request->nr_buffers &&
                pushes[i].length && pushes[i].length <= UINT32_MAX &&
                pushes[i].offset <= UINT32_MAX) {
               struct nvx_drm_device_slot *map_device;
               struct nvx_drm_bo_slot *map_slot;
               uint32_t allocation_bytes = 0;
               uint32_t word_count = 0;
               uint32_t command_words[NVX_3D_MAX_PUSH_DWORDS];
               uint32_t map_result;
               uint32_t map_count_snap = 0, map_access_snap = 0;
               uint32_t handle = buffers[pushes[i].bo_index].handle;

               /* Read the existing CPU shadow directly under the DRM lock
                * instead of taking a new map.  Mesa already holds the BO
                * mapped WRITE-only for building commands; reading the
                * shadow is safe because the pushbuf is fully built before
                * this ioctl and no concurrent writer exists. */
               nvx_drm_lock_acquire();
               map_device = nvx_drm_find_device_locked(fd);
               map_slot = nvx_drm_find_bo_locked(map_device, handle);
               if (map_slot && map_slot->bo.allocation.Bytes <= UINT32_MAX)
                  allocation_bytes = (uint32_t)map_slot->bo.allocation.Bytes;
               if (map_slot) {
                  map_count_snap = map_slot->bo.map_count;
                  map_access_snap = map_slot->bo.map_access;
               }
               if (!allocation_bytes || pushes[i].offset > allocation_bytes ||
                   pushes[i].length > allocation_bytes - pushes[i].offset) {
                  map_result = 1U;
               } else if (map_slot && map_slot->bo.shadow &&
                          map_slot->bo.map_count) {
                  /* Copy the bounded command prefix while holding the lock:
                   * another thread may unmap/free the shadow after unlock. */
                  word_count = (uint32_t)(pushes[i].length /
                                          sizeof(uint32_t));
                  if (word_count > NVX_3D_MAX_PUSH_DWORDS)
                     word_count = NVX_3D_MAX_PUSH_DWORDS;
                  memcpy(command_words, map_slot->bo.shadow +
                         (uint32_t)pushes[i].offset,
                         (size_t)word_count * sizeof(command_words[0]));
                  map_result = 0U;
               } else {
                  map_result = 2U;
               }
               nvx_drm_lock_release();

               NvxWglRecordNouveauPushbufItem(5U, i, handle,
                  allocation_bytes, (uint32_t)pushes[i].offset,
                  (uint32_t)pushes[i].length, map_result,
                  (map_count_snap << 16) | map_access_snap);
               if (!map_result) {
                  NvxWglRecordNouveauPushbufDwords(i, 0U, command_words,
                                                   word_count);
               }
            }
         }
      }
      if (relocs && request->nr_relocs <= NOUVEAU_GEM_MAX_RELOCS) {
         for (i = 0; i < request->nr_relocs && i < 16U; i++) {
            NvxWglRecordNouveauPushbufItem(3U, i, relocs[i].reloc_bo_index,
               relocs[i].reloc_bo_offset, relocs[i].bo_index,
               relocs[i].flags, relocs[i].data, relocs[i].vor);
         }
      }
      if (nvx_drm_submit_pushbuf(fd, request)) {
         NvxWglRecordNouveauPushbuf(request->channel, request->nr_push,
                                    request->nr_buffers, request->nr_relocs);
         return 0;
      }
      return nvx_drm_unsupported_operation(
         NVX_XF86DRM_UNSUPPORTED_WRITE_READ, DRM_NOUVEAU_GEM_PUSHBUF);
   }
#else
      return nvx_drm_unsupported_operation(
         NVX_XF86DRM_UNSUPPORTED_WRITE_READ, DRM_NOUVEAU_GEM_PUSHBUF);
#endif

   nvx_drm_lock_acquire();
   device = nvx_drm_find_device_locked(fd);
   if (!device || !device->device.NouveauChannelHandle ||
       !device->device.NouveauGraphicsObjectHandle ||
       request->channel != device->device.NouveauChannelHandle) {
      nvx_drm_lock_release();
      errno = device ? EINVAL : ENODEV;
      return -1;
   }

   request->suffix0 = 0;
   request->suffix1 = 0;
   request->vram_available = device->device.caps.VramApertureBytes;
   request->gart_available = device->device.caps.GartBytes;
   nvx_drm_lock_release();
   return 0;
}

static void
nvx_drm_lock_acquire(void)
{
   while (__sync_lock_test_and_set(&nvx_drm_lock, 1))
      ;
}

static void
nvx_drm_lock_release(void)
{
   __sync_lock_release(&nvx_drm_lock);
}

static int
nvx_drm_unsupported(void)
{
   errno = ENOSYS;
   return -1;
}

static int
nvx_drm_unsupported_operation(uint32_t kind, unsigned long operation)
{
   nvx_drm_lock_acquire();
   nvx_drm_status.UnsupportedCount++;
   nvx_drm_status.LastUnsupportedKind = kind;
   nvx_drm_status.LastUnsupportedOperation = (uint32_t)operation;
   nvx_drm_lock_release();
   return nvx_drm_unsupported();
}

static int
nvx_drm_lookup(int fd, NVX_WINSYS_DEVICE *device)
{
   unsigned i;
   int found = 0;

   if (!device)
      return 0;
   nvx_drm_lock_acquire();
   for (i = 0; i < NVX_DRM_DEVICE_SLOTS; i++) {
      if (nvx_drm_devices[i].fd == fd) {
         *device = nvx_drm_devices[i].device;
         found = 1;
         break;
      }
   }
   nvx_drm_lock_release();
   return found;
}

static char *
nvx_drm_strdup(const char *text)
{
   size_t bytes;
   char *copy;

   if (!text)
      return NULL;
   bytes = strlen(text) + 1U;
   copy = (char *)malloc(bytes);
   if (copy)
      memcpy(copy, text, bytes);
   return copy;
}

int
NvxXf86DrmRegisterDevice(const NVX_WINSYS_DEVICE *device)
{
   unsigned i;
   int fd = -1;

   if (!device || !device->escape || !device->opaque ||
       device->caps.Version != NVX_3D_ABI_VERSION ||
       device->caps.Size != sizeof(device->caps) ||
       device->caps.GraphicsClass != 0xc797U ||
       !device->caps.DeviceId || !device->caps.Chipset ||
       !(device->caps.PciLocation & NVX_3D_PCI_LOCATION_VALID) ||
       !device->caps.VramApertureBytes || !device->caps.MaxBufferBytes ||
       !device->ClientToken)
      return -1;

   nvx_drm_lock_acquire();
   for (i = 0; i < NVX_DRM_DEVICE_SLOTS; i++) {
      if (nvx_drm_devices[i].fd == 0) {
         fd = NVX_DRM_FD_BASE + (int)i;
         memset(&nvx_drm_devices[i], 0, sizeof(nvx_drm_devices[i]));
         nvx_drm_devices[i].fd = fd;
         nvx_drm_devices[i].device = *device;
         nvx_drm_devices[i].client_token = device->ClientToken;
         nvx_drm_devices[i].next_handle = 1U;
         break;
      }
   }
   nvx_drm_lock_release();
   if (fd < 0)
      errno = EMFILE;
   return fd;
}

void
NvxXf86DrmUnregisterDevice(int fd)
{
   unsigned i;
   nvx_drm_lock_acquire();
   for (i = 0; i < NVX_DRM_DEVICE_SLOTS; i++) {
      if (nvx_drm_devices[i].fd == fd) {
         unsigned j;
         int all_freed = 1;
         for (j = 0; j < NVX_DRM_BUFFER_SLOTS; j++) {
            struct nvx_drm_bo_slot *buffer = &nvx_drm_devices[i].buffers[j];
            if (!buffer->handle)
               continue;
            if (!NvxNouveauBoDestroy(&buffer->bo)) {
               all_freed = 0;
               continue;
            }
            memset(buffer, 0, sizeof(*buffer));
         }
         if (!all_freed)
            break;
         if (!NvxWinsysCloseClient(&nvx_drm_devices[i].device))
            break;
         memset(&nvx_drm_devices[i], 0, sizeof(nvx_drm_devices[i]));
         break;
      }
   }
   nvx_drm_lock_release();
}

int
NvxXf86DrmIsRegistered(int fd)
{
   NVX_WINSYS_DEVICE device;
   return nvx_drm_lookup(fd, &device);
}

void
NvxXf86DrmGetStatus(NVX_XF86DRM_STATUS *status)
{
   if (!status)
      return;
   nvx_drm_lock_acquire();
   *status = nvx_drm_status;
   nvx_drm_lock_release();
}

/* Read a triangle's captured target BO through the miniport CE path,
 * independently of glReadPixels and CPU shadows. */
#if defined(_WIN32) || defined(_WIN64)
__declspec(dllexport) int __stdcall
NvxXf86DrmReadDrawTargetRaw(uint32_t draw_kind, uint32_t *green_pixels,
                            uint32_t *blue_rgba_pixels,
                            uint32_t *blue_bgra_pixels, uint32_t *bytes,
                            uint64_t *target_va, uint64_t *buffer_id)
{
   NVX_WINSYS_DEVICE winsys;
   uint64_t requested_target_va = 0;
   uint64_t target_buffer_id = 0;
   uint32_t target_bytes = 0;
   uint8_t *raw;
   unsigned i, j;
   int found = 0;

   if (green_pixels)
      *green_pixels = 0;
   if (blue_rgba_pixels)
      *blue_rgba_pixels = 0;
   if (blue_bgra_pixels)
      *blue_bgra_pixels = 0;
   if (bytes)
      *bytes = 0;
   if (target_va)
      *target_va = 0;
   if (buffer_id)
      *buffer_id = 0;
   if (!green_pixels || !blue_rgba_pixels || !blue_bgra_pixels ||
       !bytes || !target_va ||
       (draw_kind != 2U && draw_kind != 3U && draw_kind != 4U))
      return 0;

   nvx_drm_lock_acquire();
   for (i = 0; i < NVX_DRM_DEVICE_SLOTS && !found; i++) {
      struct nvx_drm_device_slot *device = &nvx_drm_devices[i];
      if (device->fd < 0 || !device->device.escape ||
          !device->device.ClientToken)
         continue;
      requested_target_va = draw_kind == 2U ?
         device->shader_triangle_target_address :
         draw_kind == 3U ? device->shader_ffvs_target_address :
         device->shader_full_target_address;
      if (!requested_target_va)
         continue;
      for (j = 0; j < NVX_DRM_BUFFER_SLOTS; j++) {
         struct nvx_drm_bo_slot *slot = &device->buffers[j];
         if (!slot->handle ||
             slot->bo.allocation.GpuVa != requested_target_va ||
             slot->bo.allocation.ClientToken != device->device.ClientToken ||
             !slot->bo.allocation.BufferId ||
             !slot->bo.allocation.Bytes ||
             slot->bo.allocation.Bytes > 16U * 1024U * 1024U)
            continue;
         winsys = device->device;
         target_buffer_id = slot->bo.allocation.BufferId;
         target_bytes = (uint32_t)slot->bo.allocation.Bytes;
         *target_va = requested_target_va;
         if (buffer_id)
            *buffer_id = target_buffer_id;
         found = 1;
         break;
      }
   }
   nvx_drm_lock_release();

   if (!found)
      return 0;
   raw = (uint8_t *)malloc(target_bytes);
   if (!raw)
      return 0;
   if (!NvxWinsysDownloadBuffer(&winsys, target_buffer_id, target_bytes, 0,
                                raw, target_bytes)) {
      free(raw);
      return 0;
   }
   for (i = 0; i + sizeof(uint32_t) <= target_bytes; i += sizeof(uint32_t)) {
      uint32_t pixel;
      memcpy(&pixel, raw + i, sizeof(pixel));
      if ((pixel & 0x00ffffffU) == 0x0000ff00U)
         (*green_pixels)++;
      else if ((pixel & 0x00ffffffU) == 0x00bf0000U)
         (*blue_rgba_pixels)++;
      else if ((pixel & 0x00ffffffU) == 0x000000bfU)
         (*blue_bgra_pixels)++;
   }
   free(raw);
   *bytes = target_bytes;
   return 1;
}
#endif

static int
nvx_drm_gem_new(struct nvx_drm_device_slot *device,
                struct drm_nouveau_gem_new *request)
{
   struct nvx_drm_bo_slot *slot = NULL;
   NVX_3D_ALLOC_BUFFER description;
   uint64_t requested_bytes;
   uint32_t allocated_bytes, handle;
   uint32_t usage = 0;
   uint32_t domain = NOUVEAU_GEM_DOMAIN_VRAM;
   int allocated;
   unsigned i;

#if defined(_WIN32) || defined(_WIN64)
   /* Large NVC0 miptree BOs identify depth/target allocation failures in the
    * XP trace. Preserve the exact Nouveau placement request before the shim's
    * restricted GEM_NEW checks can reject it. */
   if (request && request->info.size >= (1U << 20))
      NvxWglRecordNouveauPushbufItem(7U, 0U,
         (uint32_t)request->info.size,
         (uint32_t)(request->info.size >> 32), request->align,
         request->info.domain, request->info.tile_mode,
         request->info.tile_flags);
#endif

   if (!device || !request || request->info.handle ||
       !request->info.size ||
       request->info.size > device->device.caps.MaxBufferBytes ||
       request->info.size > UINT32_MAX ||
       request->align > ((request->info.domain & NOUVEAU_GEM_DOMAIN_VRAM) ?
                         NVX_3D_VRAM_ALLOCATION_ALIGNMENT :
                         ((request->info.domain & NOUVEAU_GEM_DOMAIN_GART) ?
                          0x00200000U : 0x10000U)) ||
       (request->align &&
        (request->align & (request->align - 1U))) ||
       (request->info.tile_mode & ~0x70U) ||
       (request->info.tile_flags & ~NOUVEAU_GEM_TILE_NONCONTIG) ||
       (request->info.domain & (NOUVEAU_GEM_DOMAIN_CPU |
                                NOUVEAU_GEM_DOMAIN_COHERENT)) ||
       (request->info.domain &&
        !(request->info.domain & (NOUVEAU_GEM_DOMAIN_VRAM |
                                  NOUVEAU_GEM_DOMAIN_GART))))
      return 0;

   if (!(request->info.domain & NOUVEAU_GEM_DOMAIN_VRAM)) {
      if (!device->device.caps.GartBytes)
         return 0;
      usage = NVX_3D_USAGE_GART_BUFFER;
      domain = NOUVEAU_GEM_DOMAIN_GART;
   }

   requested_bytes = request->info.size;
   if (requested_bytes > UINT32_MAX - (NVX_3D_PAGE_BYTES - 1U))
      return 0;
   allocated_bytes = Nvx3dAlignU32((uint32_t)requested_bytes,
                                   NVX_3D_PAGE_BYTES);
   if (allocated_bytes > device->device.caps.MaxBufferBytes)
      return 0;

   for (i = 0; i < NVX_DRM_BUFFER_SLOTS; i++) {
      if (!device->buffers[i].handle) {
         slot = &device->buffers[i];
         break;
      }
   }
   if (!slot)
      return 0;

   memset(&description, 0, sizeof(description));
   description.Size = sizeof(description);
   description.Version = NVX_3D_ABI_VERSION;
   description.Usage = usage;
   description.Bytes = allocated_bytes;
   description.Alignment = request->align ? request->align : 0x1000U;
   handle = device->next_handle++;
   if (!handle)
      handle = device->next_handle++;

   allocated = NvxNouveauBoCreate(&device->device, &description,
                                  device->client_token, &slot->bo);
   if (!allocated && (request->info.domain & NOUVEAU_GEM_DOMAIN_VRAM) &&
       device->device.caps.GartBytes >= requested_bytes) {
      /* NVC0's per-GPC TLS BO can exceed the small, firmware-confirmed free
       * BAR1 VRAM window while still fitting the mapped GART. Keep placement
       * truthful by retrying in system memory and reporting GART to Mesa. */
      description.Usage = NVX_3D_USAGE_GART_BUFFER;
      allocated = NvxNouveauBoCreate(&device->device, &description,
                                     device->client_token, &slot->bo);
      if (allocated)
         domain = NOUVEAU_GEM_DOMAIN_GART;
   }
   if (!allocated) {
#if defined(_WIN32) || defined(_WIN64)
      if (requested_bytes >= (1U << 20))
         NvxWglRecordNouveauPushbufItem(8U, 0U, 0U, domain,
            (uint32_t)requested_bytes, allocated_bytes, request->align, 0U);
#endif
      return 0;
   }

   /* The miniport places VRAM BOs at 128 KiB boundaries and GART BOs at
    * 2 MiB boundaries. Confirm every requested VA alignment; never claim the
    * NVX allocator/MMU created a Nouveau tiled or compressed layout. */
   if (request->align &&
       (slot->bo.allocation.GpuVa & (request->align - 1U))) {
      (void)NvxNouveauBoDestroy(&slot->bo);
      memset(&slot->bo, 0, sizeof(slot->bo));
#if defined(_WIN32) || defined(_WIN64)
      if (requested_bytes >= (1U << 20))
         NvxWglRecordNouveauPushbufItem(8U, 0U, 0U, domain,
            (uint32_t)requested_bytes, allocated_bytes, request->align, 1U);
#endif
      return 0;
   }

#if defined(_WIN32) || defined(_WIN64)
   if (requested_bytes >= (1U << 20))
      NvxWglRecordNouveauPushbufItem(8U, 0U, 1U, domain,
         (uint32_t)requested_bytes, allocated_bytes, request->align, 0U);
#endif

   slot->handle = handle;
   request->info.handle = handle;
   request->info.domain = domain |
                          NOUVEAU_GEM_DOMAIN_MAPPABLE;
   request->info.size = requested_bytes;
   request->info.offset = slot->bo.allocation.GpuVa;
   request->info.map_handle = handle;
   request->info.tile_flags |= NOUVEAU_GEM_TILE_NONCONTIG;
   return 1;
}

static int
nvx_drm_gem_info(struct nvx_drm_device_slot *device,
                 struct drm_nouveau_gem_info *info)
{
   struct nvx_drm_bo_slot *slot;

   if (!device || !info || !(slot = nvx_drm_find_bo_locked(device,
                                                            info->handle)))
      return 0;
   info->domain = (slot->bo.allocation.Usage & NVX_3D_USAGE_GART_BUFFER ?
                   NOUVEAU_GEM_DOMAIN_GART : NOUVEAU_GEM_DOMAIN_VRAM) |
                  NOUVEAU_GEM_DOMAIN_MAPPABLE;
   info->size = slot->bo.allocation.Bytes;
   info->offset = slot->bo.allocation.GpuVa;
   info->map_handle = slot->handle;
   info->tile_flags |= NOUVEAU_GEM_TILE_NONCONTIG;
   return 1;
}

void *
NvxXf86DrmMapBuffer(int fd, uint32_t handle, uint32_t access)
{
   struct nvx_drm_device_slot *device;
   struct nvx_drm_bo_slot *slot;
   void *map = NULL;
   int found;

   nvx_drm_lock_acquire();
   device = nvx_drm_find_device_locked(fd);
   slot = nvx_drm_find_bo_locked(device, handle);
   found = slot != NULL;
   if (slot)
      map = NvxNouveauBoMap(&slot->bo, access);
   nvx_drm_lock_release();
   if (!map)
      errno = found ? EIO : ENOENT;
   return map;
}

int
NvxXf86DrmUpdateBufferMapAccess(int fd, uint32_t handle, uint32_t access)
{
   struct nvx_drm_device_slot *device;
   struct nvx_drm_bo_slot *slot;
   uint32_t bo_access = 0U;
   int result = 0;
   int found;

   if (!access || (access & ~(NVX_XF86DRM_MAP_READ |
                              NVX_XF86DRM_MAP_WRITE))) {
      errno = EINVAL;
      return -1;
   }
   if (access & NVX_XF86DRM_MAP_READ)
      bo_access |= NVX_NOUVEAU_BO_READ;
   if (access & NVX_XF86DRM_MAP_WRITE)
      bo_access |= NVX_NOUVEAU_BO_WRITE;

   nvx_drm_lock_acquire();
   device = nvx_drm_find_device_locked(fd);
   slot = nvx_drm_find_bo_locked(device, handle);
   found = slot != NULL;
   if (slot && slot->bo.map_count) {
      slot->bo.map_access |= bo_access;
      result = 1;
   }
   nvx_drm_lock_release();
   if (!result)
      errno = found ? EINVAL : ENOENT;
   return result ? 0 : -1;
}

int
NvxXf86DrmUnmapBuffer(int fd, uint32_t handle)
{
   struct nvx_drm_device_slot *device;
   struct nvx_drm_bo_slot *slot;
   int result = 0;
   int found;

   nvx_drm_lock_acquire();
   device = nvx_drm_find_device_locked(fd);
   slot = nvx_drm_find_bo_locked(device, handle);
   found = slot != NULL;
   if (slot)
      result = NvxNouveauBoUnmap(&slot->bo);
   nvx_drm_lock_release();
   if (!result)
      errno = found ? EIO : ENOENT;
   return result ? 0 : -1;
}

int
NvxXf86DrmFlushBuffer(int fd, uint32_t handle)
{
   struct nvx_drm_device_slot *device;
   struct nvx_drm_bo_slot *slot;
   int result = 0;
   int found;

   nvx_drm_lock_acquire();
   device = nvx_drm_find_device_locked(fd);
   slot = nvx_drm_find_bo_locked(device, handle);
   found = slot != NULL;
   if (slot)
      result = NvxNouveauBoFlush(&slot->bo);
   nvx_drm_lock_release();
   if (!result)
      errno = found ? EIO : ENOENT;
   return result ? 0 : -1;
}

drmVersionPtr
drmGetVersion(int fd)
{
   drmVersionPtr version;

   if (!NvxXf86DrmIsRegistered(fd)) {
      errno = ENODEV;
      return NULL;
   }
   version = (drmVersionPtr)calloc(1, sizeof(*version));
   if (!version)
      return NULL;
   version->name = nvx_drm_strdup("nvx");
   if (!version->name) {
      free(version);
      return NULL;
   }
   version->name_len = 3;
   /* Zero version denotes the NVX transport; it is not a claimed Nouveau DRM
    * kernel version. Nouveau's XP path checks registration directly. */
   return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
   if (!version)
      return;
   free(version->name);
   free(version->date);
   free(version->desc);
   free(version);
}

int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *out_device)
{
   NVX_WINSYS_DEVICE winsys;
   drmDevicePtr device;
   drmPciBusInfoPtr bus;
   drmPciDeviceInfoPtr info;

   if (!out_device || flags || !nvx_drm_lookup(fd, &winsys) ||
       !(winsys.caps.PciLocation & NVX_3D_PCI_LOCATION_VALID)) {
      if (out_device)
         *out_device = NULL;
      errno = ENODEV;
      return -1;
   }
   *out_device = NULL;

   device = (drmDevicePtr)calloc(1, sizeof(*device));
   bus = (drmPciBusInfoPtr)calloc(1, sizeof(*bus));
   info = (drmPciDeviceInfoPtr)calloc(1, sizeof(*info));
   if (!device || !bus || !info)
      goto fail;

   bus->domain = 0;
   bus->bus = (uint8_t)(winsys.caps.PciLocation &
                        NVX_3D_PCI_LOCATION_BUS_MASK);
   bus->dev = (uint8_t)((winsys.caps.PciLocation >>
                         NVX_3D_PCI_LOCATION_SLOT_SHIFT) & 0x1fU);
   bus->func = (uint8_t)((winsys.caps.PciLocation >>
                          (NVX_3D_PCI_LOCATION_SLOT_SHIFT + 5U)) & 0x7U);
   info->vendor_id = 0x10deU;
   info->device_id = (uint16_t)winsys.caps.DeviceId;
   device->bustype = DRM_BUS_PCI;
   device->businfo.pci = bus;
   device->deviceinfo.pci = info;
   *out_device = device;
   return 0;

fail:
   free(info);
   free(bus);
   free(device);
   errno = ENOMEM;
   return -1;
}

void
drmFreeDevice(drmDevicePtr *device)
{
   unsigned i;
   if (!device || !*device)
      return;
   if ((*device)->bustype == DRM_BUS_PLATFORM) {
      drmPlatformDeviceInfoPtr info = (*device)->deviceinfo.platform;
      if (info) {
         if (info->compatible) {
            for (i = 0; info->compatible[i]; i++)
               free(info->compatible[i]);
         }
         free(info->compatible);
         free(info);
      }
      free((*device)->businfo.platform);
   } else if ((*device)->bustype == DRM_BUS_PCI) {
      free((*device)->businfo.pci);
      free((*device)->deviceinfo.pci);
   }
   free((*device)->nodes);
   free(*device);
   *device = NULL;
}

int
drmCommandWrite(int fd, unsigned long command, void *data,
                unsigned long size)
{
   if (command == DRM_NOUVEAU_CHANNEL_FREE)
      return nvx_drm_channel_free(fd, data, size);
   if (command == DRM_NOUVEAU_NVIF)
      return nvx_drm_nvif_write(fd, data, size);
   if (command == DRM_NOUVEAU_GEM_CPU_PREP) {
      struct drm_nouveau_gem_cpu_prep *request = data;
      struct nvx_drm_device_slot *device;
      struct nvx_drm_bo_slot *buffer;
      int found;
      int ok = 0;

      if (!request || size != sizeof(*request))
         return nvx_drm_unsupported_operation(
            NVX_XF86DRM_UNSUPPORTED_WRITE, command);
      nvx_drm_lock_acquire();
      device = nvx_drm_find_device_locked(fd);
      buffer = nvx_drm_find_bo_locked(device, request->handle);
      found = buffer != NULL;
      if (buffer) {
         ok = !buffer->bo.last_fence ||
              (!(request->flags & NOUVEAU_GEM_CPU_PREP_NOWAIT) &&
               NvxWinsysWaitFence(&device->device, buffer->bo.last_fence,
                                  5000U));
         /* Mesa keeps the CPU shadow mapped across accesses. Waiting for the
          * GPU fence alone leaves that mapping stale after render-target
          * writes; refresh it before reporting a read-prep as complete. */
         if (ok && !(request->flags & NOUVEAU_GEM_CPU_PREP_WRITE) &&
             buffer->bo.shadow && buffer->bo.map_count) {
            uint32_t stale = buffer->bo.shadow_fence <
                             buffer->bo.last_fence;
            ok = NvxNouveauBoRefreshRead(&buffer->bo);
#if defined(_WIN32) || defined(_WIN64)
            if (nvx_drm_prep_trace_count < 48U) {
               uint32_t trace_index = nvx_drm_prep_trace_count++;
               NvxWglRecordNouveauPushbufItem(10U, trace_index,
                  request->handle, (uint32_t)buffer->bo.last_fence,
                  (uint32_t)buffer->bo.shadow_fence, stale,
                  (uint32_t)ok, request->flags);
            }
#else
            (void)stale;
#endif
         }
      }
#if defined(_WIN32) || defined(_WIN64)
      if (!(request->flags & NOUVEAU_GEM_CPU_PREP_WRITE) &&
          nvx_drm_prep_call_trace_count < 48U) {
         uint32_t trace_index = nvx_drm_prep_call_trace_count++;
         NvxWglRecordNouveauPushbufItem(11U, trace_index,
            request->handle, request->flags, (uint32_t)found,
            buffer ? buffer->bo.map_count : 0U,
            buffer ? (uint32_t)buffer->bo.last_fence : 0U,
            buffer ? (uint32_t)buffer->bo.shadow_fence : (uint32_t)ok);
      }
#endif
      nvx_drm_lock_release();
      if (ok)
         return 0;
      errno = found ? EBUSY : ENOENT;
      return -1;
   }
   if (command == DRM_NOUVEAU_GEM_CPU_FINI) {
      struct drm_nouveau_gem_cpu_fini *request = data;
      struct nvx_drm_device_slot *device;
      struct nvx_drm_bo_slot *buffer;
      int found;
      int ok = 0;

      if (!request || size != sizeof(*request))
         return nvx_drm_unsupported_operation(
            NVX_XF86DRM_UNSUPPORTED_WRITE, command);
      nvx_drm_lock_acquire();
      device = nvx_drm_find_device_locked(fd);
      buffer = nvx_drm_find_bo_locked(device, request->handle);
      found = buffer != NULL;
      if (buffer)
         ok = NvxNouveauBoFlush(&buffer->bo);
      nvx_drm_lock_release();
      if (ok)
         return 0;
      errno = found ? EIO : ENOENT;
      return -1;
   }
   return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_WRITE,
                                       command);
}

int
drmCommandWriteRead(int fd, unsigned long command, void *data,
                    unsigned long size)
{
   struct nvx_drm_device_slot *device;
   int ok;

   if (command == DRM_NOUVEAU_GETPARAM)
      return nvx_drm_getparam(fd, data, size);
   if (command == DRM_NOUVEAU_CHANNEL_ALLOC)
      return nvx_drm_channel_alloc(fd, data, size);
   if (command == DRM_NOUVEAU_GEM_PUSHBUF)
      return nvx_drm_pushbuf_nop(fd, data, size);
   if (command == DRM_NOUVEAU_NVIF)
      {
         struct nvif_ioctl_v0 *ioctl = data;
#if defined(_WIN32) || defined(_WIN64)
         uint32_t method = 0;
         if (ioctl && ioctl->type == NVIF_IOCTL_V0_MTHD &&
             size >= sizeof(*ioctl) + sizeof(struct nvif_ioctl_mthd_v0)) {
            const struct nvif_ioctl_mthd_v0 *mthd =
               (const struct nvif_ioctl_mthd_v0 *)
                  ((const uint8_t *)data + sizeof(*ioctl));
            method = mthd->method;
         }
         if (ioctl)
            NvxWglRecordNouveauIoctl(ioctl->type, ioctl->version,
                                     ioctl->route, method,
                                     (uint32_t)size);
#endif
         if (ioctl && ioctl->type == NVIF_IOCTL_V0_SCLASS)
            return nvx_drm_nvif_sclass(fd, data, size);
         return nvx_drm_nvif_device_info(fd, data, size);
      }
   if (command == DRM_NOUVEAU_GEM_NEW) {
      struct drm_nouveau_gem_new *request = data;
      int found;
      if (!request || size != sizeof(*request))
         return nvx_drm_unsupported_operation(
            NVX_XF86DRM_UNSUPPORTED_WRITE_READ, command);
      nvx_drm_lock_acquire();
      device = nvx_drm_find_device_locked(fd);
      found = device != NULL;
      nvx_drm_status.GemNewCount++;
      nvx_drm_status.LastGemNewSize = request->info.size;
      nvx_drm_status.LastGemNewTileMode = request->info.tile_mode;
      nvx_drm_status.LastGemNewTileFlags = request->info.tile_flags;
      ok = nvx_drm_gem_new(device, request);
      nvx_drm_status.LastGemNewDomain = request->info.domain;
      nvx_drm_status.LastGemNewResult = ok ? 0U : (found ? 1U : 2U);
      nvx_drm_lock_release();
      if (ok)
         return 0;
      errno = found ? ENOMEM : ENODEV;
      return -1;
   }
   if (command == DRM_NOUVEAU_GEM_INFO) {
      struct drm_nouveau_gem_info *info = data;
      int found;
      if (!info || size != sizeof(*info))
         return nvx_drm_unsupported_operation(
            NVX_XF86DRM_UNSUPPORTED_WRITE_READ, command);
      nvx_drm_lock_acquire();
      device = nvx_drm_find_device_locked(fd);
      found = device != NULL;
      ok = nvx_drm_gem_info(device, info);
      nvx_drm_lock_release();
      if (ok)
         return 0;
      errno = found ? ENOENT : ENODEV;
      return -1;
   }
   return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_WRITE_READ,
                                       command);
}

int
drmCommandRead(int fd, unsigned long command, void *data,
               unsigned long size)
{
   (void)fd;
   (void)data;
   (void)size;
   return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_READ,
                                       command);
}

int
drmCommandNone(int fd, unsigned long command)
{
   (void)fd;
   return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_NONE,
                                       command);
}

int
drmIoctl(int fd, unsigned long request, void *data)
{
   (void)fd;
   (void)data;
   return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_IOCTL,
                                       request);
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
   struct nvx_drm_device_slot *device;
   struct nvx_drm_bo_slot *buffer;
   int result = 0;
   int found;

   nvx_drm_lock_acquire();
   device = nvx_drm_find_device_locked(fd);
   buffer = nvx_drm_find_bo_locked(device, handle);
   found = buffer != NULL;
   if (buffer && NvxNouveauBoDestroy(&buffer->bo)) {
      memset(buffer, 0, sizeof(*buffer));
      result = 1;
   }
   nvx_drm_lock_release();
   if (!result)
      errno = found ? EIO : ENOENT;
   return result ? 0 : -1;
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
   (void)fd;
   if (handle)
      *handle = 0;
   return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_PRIME_TO_HANDLE,
                                       (uint32_t)prime_fd);
}

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
   (void)fd;
   (void)flags;
   if (prime_fd)
      *prime_fd = -1;
   return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_HANDLE_TO_PRIME,
                                       handle);
}
