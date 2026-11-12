#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nvx_winsys.h"
#include "nvx_nouveau_bo.h"
#include "nvx_nouveau_device.h"
#include "nvx_nouveau_pushbuf.h"
#include "nvx_nouveau_screen.h"

#define MOCK_MAX_BUFFERS 8U

struct mock_buffer {
   uint64_t id;
   uint64_t gpu_va;
   uint64_t client_token;
   uint32_t bytes;
   uint32_t usage;
   uint32_t width;
   uint32_t height;
   uint32_t pitch_bytes;
   uint32_t tile_mode;
   uint8_t *storage;
};

struct mock_state {
   uint64_t client_token;
   uint64_t next_buffer;
   uint64_t freed_buffer;
   uint64_t waited_fence;
   uint64_t submitted_fence;
   uint32_t upload_count;
   uint32_t upload_bytes;
   uint32_t download_count;
   uint32_t download_bytes;
   uint32_t event_count;
   uint32_t events[256];
   int fail_wait;
   struct mock_buffer buffers[MOCK_MAX_BUFFERS];
};

enum mock_event {
   MOCK_EVENT_ALLOC = 1,
   MOCK_EVENT_UPLOAD,
   MOCK_EVENT_DOWNLOAD,
   MOCK_EVENT_FREE,
   MOCK_EVENT_WAIT,
   MOCK_EVENT_VALIDATE,
   MOCK_EVENT_SUBMIT
};

static void
record_event(struct mock_state *mock, uint32_t event)
{
   assert(mock->event_count < sizeof(mock->events) / sizeof(mock->events[0]));
   mock->events[mock->event_count++] = event;
}

static struct mock_buffer *
find_buffer(struct mock_state *mock, uint64_t id)
{
   uint32_t i;
   for (i = 0; i < MOCK_MAX_BUFFERS; i++) {
      if (mock->buffers[i].id == id)
         return &mock->buffers[i];
   }
   return NULL;
}

static int
mock_escape(void *opaque, uint32_t escape, uint32_t input_bytes,
            const void *input, uint32_t output_bytes, void *output)
{
   struct mock_state *mock = opaque;

   switch (escape) {
   case NVX_ESCAPE_3D_CLIENT_OPEN: {
      NVX_3D_CLIENT_SESSION *session = output;
      assert(input_bytes == sizeof(*session) && output_bytes == sizeof(*session));
      *session = *(const NVX_3D_CLIENT_SESSION *)input;
      assert(Nvx3dValidateClientSession(session, NVX_3D_CLIENT_OPEN));
      session->ClientToken = mock->client_token = 0x123400000077ULL;
      return 1;
   }
   case NVX_ESCAPE_3D_CLIENT_CLOSE: {
      const NVX_3D_CLIENT_SESSION *session = input;
      assert(input_bytes == sizeof(*session) && output_bytes == 0);
      assert(Nvx3dValidateClientSession(session, NVX_3D_CLIENT_CLOSE));
      assert(session->ClientToken == mock->client_token);
      mock->client_token = 0;
      return 1;
   }
   case NVX_ESCAPE_3D_QUERY_CAPS: {
      NVX_3D_QUERY_CAPS *caps = output;
      assert(input_bytes == 0 && input == NULL);
      assert(output_bytes >= sizeof(*caps));
      caps->Features = NVX_3D_FEATURE_USER_ALLOC | NVX_3D_FEATURE_UPLOAD |
                       NVX_3D_FEATURE_DOWNLOAD | NVX_3D_FEATURE_GR_CHANNEL |
                       NVX_3D_FEATURE_FIXED_PROFILE_SUBMIT;
      caps->GraphicsClass = 0xc797;
      caps->MaxPushDwords = NVX_3D_MAX_PUSH_DWORDS;
      caps->PageBytes = NVX_3D_PAGE_BYTES;
      caps->MaxBufferBytes = NVX_3D_MAX_BUFFER_BYTES;
      caps->Chipset = 0x170;
      caps->DeviceId = 0x2484;
      caps->VramApertureBytes = NVX_3D_MAX_BUFFER_BYTES;
      caps->GartBytes = 0;
      return 1;
   }
   case NVX_ESCAPE_3D_ALLOC: {
      NVX_3D_ALLOC_BUFFER *request = (NVX_3D_ALLOC_BUFFER *)input;
      NVX_3D_ALLOC_BUFFER *result = output;
      struct mock_buffer *buffer = NULL;
      uint32_t i;
      assert(input_bytes == sizeof(*request));
      assert(output_bytes >= sizeof(*result));
      for (i = 0; i < MOCK_MAX_BUFFERS; i++) {
         if (!mock->buffers[i].id) {
            buffer = &mock->buffers[i];
            break;
         }
      }
      assert(buffer != NULL);
      *result = *request;
      result->BufferId = ++mock->next_buffer;
      result->GpuVa = 0x24000000ULL + result->BufferId * 0x100000ULL;
      buffer->id = result->BufferId;
      buffer->gpu_va = result->GpuVa;
      buffer->client_token = result->ClientToken;
      buffer->bytes = request->Bytes;
      buffer->usage = request->Usage;
      buffer->width = request->Width;
      buffer->height = request->Height;
      buffer->pitch_bytes = request->PitchBytes;
      buffer->tile_mode = request->TileMode;
      buffer->storage = calloc(1, buffer->bytes);
      assert(buffer->storage != NULL);
      record_event(mock, MOCK_EVENT_ALLOC);
      return 1;
   }
   case NVX_ESCAPE_3D_UPLOAD: {
      const NVX_3D_UPLOAD_BUFFER *upload = input;
      struct mock_buffer *buffer = find_buffer(mock, upload->BufferId);
      uint32_t i;
      assert(output_bytes == 0 && output == NULL);
      assert(input_bytes == sizeof(*upload) + upload->Bytes);
      assert(buffer != NULL);
      assert(Nvx3dValidateUpload(upload, input_bytes,
                                buffer->bytes));
      for (i = 0; i < sizeof(upload->Reserved) /
                      sizeof(upload->Reserved[0]); i++)
         assert(upload->Reserved[i] == 0);
      memcpy(buffer->storage + upload->Offset,
             (const uint8_t *)input + sizeof(*upload), upload->Bytes);
      mock->upload_count++;
      mock->upload_bytes += upload->Bytes;
      record_event(mock, MOCK_EVENT_UPLOAD);
      return 1;
   }
   case NVX_ESCAPE_3D_DOWNLOAD: {
      const NVX_3D_DOWNLOAD_BUFFER *download = input;
      struct mock_buffer *buffer = find_buffer(mock, download->BufferId);
      assert(input_bytes == sizeof(*download));
      assert(output && output_bytes == download->Bytes);
      assert(buffer != NULL);
      assert(Nvx3dValidateDownload(download, input_bytes, output_bytes,
                                  buffer->bytes));
      memcpy(output, buffer->storage + download->Offset, download->Bytes);
      mock->download_count++;
      mock->download_bytes += download->Bytes;
      record_event(mock, MOCK_EVENT_DOWNLOAD);
      return 1;
   }
   case NVX_ESCAPE_3D_FREE: {
      const NVX_3D_FREE_BUFFER *request = input;
      struct mock_buffer *buffer = find_buffer(mock, request->BufferId);
      assert(input_bytes == sizeof(*request));
      assert(output_bytes == 0 && output == NULL);
      assert(Nvx3dValidateFree(request));
      assert(buffer != NULL);
      mock->freed_buffer = request->BufferId;
      record_event(mock, MOCK_EVENT_FREE);
      free(buffer->storage);
      memset(buffer, 0, sizeof(*buffer));
      return 1;
   }
   case NVX_ESCAPE_3D_WAIT: {
      const NVX_3D_WAIT_FENCE *request = input;
      NVX_3D_WAIT_FENCE *result = output;
      assert(input_bytes == sizeof(*request));
      assert(output_bytes >= sizeof(*result));
      result->Size = sizeof(*result);
      result->Version = NVX_3D_ABI_VERSION;
      result->FenceValue = request->FenceValue;
      result->Status = mock->fail_wait ? NVX_3D_STATUS_INVALID_PARAMETER :
                                        NVX_3D_STATUS_SUCCESS;
      mock->waited_fence = request->FenceValue;
      record_event(mock, MOCK_EVENT_WAIT);
      return !mock->fail_wait;
   }
   case NVX_ESCAPE_3D_VALIDATE_BATCH: {
      NVX_3D_BATCH_HEADER header;
      NVX_3D_ALLOC_BUFFER allocations[MOCK_MAX_BUFFERS];
      uint32_t allocation_count = 0;
      uint32_t i;
      assert(input && output_bytes == 0 && output == NULL);
      assert(input_bytes >= sizeof(header));
      memcpy(&header, input, sizeof(header));
      for (i = 0; i < MOCK_MAX_BUFFERS; i++) {
         struct mock_buffer *buffer = &mock->buffers[i];
         NVX_3D_ALLOC_BUFFER *allocation;
         if (!buffer->id || buffer->client_token != header.ClientToken)
            continue;
         allocation = &allocations[allocation_count++];
         memset(allocation, 0, sizeof(*allocation));
         allocation->Size = sizeof(*allocation);
         allocation->Version = NVX_3D_ABI_VERSION;
         allocation->Bytes = buffer->bytes;
         allocation->BufferId = buffer->id;
         allocation->GpuVa = buffer->gpu_va;
         allocation->ClientToken = buffer->client_token;
      }
      record_event(mock, MOCK_EVENT_VALIDATE);
      return Nvx3dValidateNvc0Batch(input, input_bytes, allocations,
                                    allocation_count, header.ClientToken);
   }
   case NVX_ESCAPE_3D_NVC0_NATIVE_CLEAR: {
      const NVX_3D_NVC0_CLEAR *request = input;
      NVX_3D_NVC0_CLEAR *result = output;
      struct mock_buffer *buffer = find_buffer(mock, request->BufferId);
      NVX_3D_COLOR_SURFACE_LAYOUT layout;
      uint32_t color;
      assert(input_bytes == sizeof(*request));
      assert(output_bytes == sizeof(*result));
      assert(buffer && buffer->client_token);
      assert(buffer->usage & NVX_3D_USAGE_COLOR_TARGET);
      assert(Nvx3dComputeColorSurfaceLayout(buffer->width, buffer->height,
                                            &layout));
      assert(layout.Bytes <= buffer->bytes);
      assert(Nvx3dValidateNvc0Clear(request, input_bytes,
                                   buffer->client_token, buffer->gpu_va,
                                   buffer->bytes, buffer->width,
                                   buffer->height, buffer->pitch_bytes,
                                   buffer->tile_mode, &color));
      *result = *request;
      result->FenceValue = ++mock->submitted_fence;
      record_event(mock, MOCK_EVENT_SUBMIT);
      return 1;
   }
   default:
      assert(!"unexpected escape");
      return 0;
   }
}

int
main(void)
{
   NVX_WINSYS_DEVICE device;
   NVX_NOUVEAU_DEVICE nouveau_device;
   NVX_NOUVEAU_SCREEN nouveau_screen;
   struct mock_state mock = {0};
   NVX_3D_ALLOC_BUFFER request = {0}, result;
   uint8_t *data;
   const uint32_t bytes = NVX_3D_MAX_UPLOAD_BYTES * 2U + 8192U;

   assert(NvxWinsysInit(&device, mock_escape, &mock));
   assert(device.ClientToken == mock.client_token);
   assert(device.caps.GraphicsClass == 0xc797);
   assert(NvxNouveauDeviceInitFromWinsys(&nouveau_device, &device));
   assert(NvxNouveauScreenInit(&nouveau_screen, &nouveau_device,
                               0x123400000077ULL));
   assert(nouveau_screen.initialized &&
          nouveau_screen.graphics_class == 0xc797 &&
          nouveau_screen.pushbuf.device == &nouveau_device.winsys &&
          nouveau_screen.pushbuf.client_token == 0x123400000077ULL);
   NvxNouveauScreenDestroy(&nouveau_screen);
   assert(!nouveau_screen.initialized);
   assert(nouveau_device.chipset == 0x170 &&
          nouveau_device.class_3d == 0xc797 &&
          nouveau_device.device_id == 0x2484 &&
          nouveau_device.vram_aperture_bytes == NVX_3D_MAX_BUFFER_BYTES &&
          nouveau_device.gart_size == 0);
   {
      NVX_WINSYS_DEVICE unsupported = device;
      unsupported.caps.DeviceId = 0x2487;
      unsupported.caps.Chipset = 0x180;
      assert(!NvxNouveauDeviceInitFromWinsys(&nouveau_device, &unsupported));
      unsupported = device;
      unsupported.caps.Chipset = 0x180;
      assert(!NvxNouveauDeviceInitFromWinsys(&nouveau_device, &unsupported));
      unsupported = device;
      unsupported.caps.Features &= ~NVX_3D_FEATURE_DOWNLOAD;
      assert(!NvxNouveauDeviceInitFromWinsys(&nouveau_device, &unsupported));
   }

   request.Size = sizeof(request);
   request.Version = NVX_3D_ABI_VERSION;
   request.Usage = 1;
   request.Bytes = bytes;
   request.ClientToken = 0x123400000001ULL;
   assert(NvxWinsysAllocBuffer(&device, &request, &result));
   assert(result.BufferId == 1 && result.GpuVa != 0);

   data = malloc(bytes);
   assert(data != NULL);
   memset(data, 0x5a, bytes);
   assert(NvxWinsysUploadBuffer(&device, result.BufferId, result.Bytes,
                                0, data, bytes));
   assert(mock.upload_count == 3 && mock.upload_bytes == bytes);
   {
      uint8_t *downloaded = malloc(bytes);
      assert(downloaded != NULL);
      memset(downloaded, 0, bytes);
      assert(NvxWinsysDownloadBuffer(&device, result.BufferId, result.Bytes,
                                     0, downloaded, bytes));
      assert(memcmp(data, downloaded, bytes) == 0);
      assert(mock.download_count ==
             (bytes + NVX_3D_MAX_DOWNLOAD_BYTES - 1U) /
             NVX_3D_MAX_DOWNLOAD_BYTES);
      assert(mock.download_bytes == bytes);
      assert(!NvxWinsysDownloadBuffer(&device, result.BufferId, result.Bytes,
                                     result.Bytes - 4, downloaded, 8));
      assert(!NvxWinsysDownloadBuffer(&device, result.BufferId, result.Bytes,
                                     2, downloaded, 4));
      free(downloaded);
   }
   assert(!NvxWinsysUploadBuffer(&device, result.BufferId, result.Bytes,
                                 result.Bytes - 4, data, 8));
   assert(!NvxWinsysUploadBuffer(&device, result.BufferId, result.Bytes,
                                 2, data, 4));
   free(data);

   assert(NvxWinsysWaitFence(&device, 42, 1000));
   assert(mock.waited_fence == 42);
   assert(NvxWinsysFreeBuffer(&device, result.BufferId));
   assert(mock.freed_buffer == result.BufferId);

   {
      NVX_3D_ALLOC_BUFFER second;
      assert(NvxWinsysAllocBuffer(&device, &request, &second));
      assert(second.BufferId != result.BufferId &&
             second.ClientToken == result.ClientToken &&
             second.AllocationToken != result.AllocationToken);
      assert(NvxWinsysFreeBuffer(&device, second.BufferId));
   }

   request.Bytes--;
   request.ClientToken++;
   assert(!NvxWinsysAllocBuffer(&device, &request, &result));

   {
      NVX_3D_ALLOC_BUFFER description = {0};
      NVX_NOUVEAU_BO bo;
      NVX_NOUVEAU_RELOCATION relocation;
      uint8_t *mapped;
      uint64_t gpu_va;
      uint32_t command_dwords[3] = { 0x20010000U, 0, 0x00000000U };

      description.Usage = 1;
      description.Bytes = 4096;
      assert(NvxNouveauBoCreate(&device, &description,
                                0x123400000002ULL, &bo));
      assert(bo.allocation.BufferId == 3 && bo.references == 1);
      assert(NvxNouveauBoMarkUsed(&bo, 77));
      assert(!NvxNouveauBoMarkUsed(&bo, 76));
      assert(NvxNouveauBoGetAddress(&bo, 16, 64,
                                   NVX_NOUVEAU_BO_READ, &gpu_va));
      assert(gpu_va == bo.allocation.GpuVa + 16);
      assert(NvxNouveauBoGetAddress(&bo, bo.allocation.Bytes - 4, 4,
                                   NVX_NOUVEAU_BO_WRITE, &gpu_va));
      assert(gpu_va == bo.allocation.GpuVa + bo.allocation.Bytes - 4);
      memset(&relocation, 0xFF, sizeof(relocation));
      assert(NvxNouveauBoBuildRelocation(&bo, command_dwords, 3, 1,
                                        128, 256, NVX_NOUVEAU_BO_READ |
                                        NVX_NOUVEAU_BO_WRITE, &relocation));
      assert(relocation.CommandWord == 1 &&
             relocation.Access == (NVX_NOUVEAU_BO_READ |
                                   NVX_NOUVEAU_BO_WRITE) &&
             relocation.BufferId == bo.allocation.BufferId &&
             relocation.BufferOffset == 128 && relocation.Bytes == 256 &&
             relocation.Reserved[0] == 0 && relocation.Reserved[1] == 0);
      assert(Nvx3dValidateRelocation(&relocation, 3, &bo.allocation));
      assert(!Nvx3dValidateRelocation(&relocation, 1, &bo.allocation));
      relocation.Reserved[0] = 1;
      assert(!Nvx3dValidateRelocation(&relocation, 3, &bo.allocation));
      assert(!NvxNouveauBoBuildRelocation(&bo, command_dwords, 3, 3,
                                         128, 256, NVX_NOUVEAU_BO_READ,
                                         &relocation));
      assert(relocation.BufferId == 0 && relocation.Bytes == 0);
      assert(!NvxNouveauBoBuildRelocation(&bo, command_dwords, 3, 1,
                                         bo.allocation.Bytes - 3, 8,
                                         NVX_NOUVEAU_BO_READ, &relocation));
      assert(relocation.BufferId == 0);
      gpu_va = UINT64_MAX;
      assert(!NvxNouveauBoGetAddress(&bo, bo.allocation.Bytes - 3, 4,
                                    NVX_NOUVEAU_BO_READ, &gpu_va));
      assert(gpu_va == 0);
      assert(!NvxNouveauBoGetAddress(&bo, 0, 0, NVX_NOUVEAU_BO_READ,
                                    &gpu_va));
      assert(!NvxNouveauBoGetAddress(&bo, 0, 4, 0, &gpu_va));
      {
         NVX_NOUVEAU_BO invalid = bo;
         invalid.allocation.GpuVa = UINT64_MAX - 7;
         assert(!NvxNouveauBoGetAddress(&invalid, 4, 8,
                                       NVX_NOUVEAU_BO_READ, &gpu_va));
         assert(gpu_va == 0);
      }

      {
         NVX_NOUVEAU_PUSHBUF push;
         uint32_t packet[6];
         packet[0] = Nvx3dNvc0SeqHeader(0x0100U, 2U);
         packet[1] = 0;
         packet[2] = 0;
         packet[3] = Nvx3dNvc0ImmediateHeader(0x0104U, 1U);
         packet[4] = Nvx3dNvc0NonIncHeader(0x0108U, 1U);
         packet[5] = 0;
         assert(NvxNouveauPushbufInit(&push, &device,
                                      bo.allocation.ClientToken));
         assert(NvxNouveauPushbufAppend(&push, packet, 6));
         assert(NvxNouveauPushbufRelocate(&push, &bo, 1, 128, 4,
                                          NVX_NOUVEAU_BO_READ));
         assert(NvxNouveauPushbufRelocate(&push, &bo, 5, 256, 4,
                                          NVX_NOUVEAU_BO_WRITE));
         mock.event_count = 0;
         assert(NvxNouveauPushbufValidate(&push));
         assert(mock.event_count == 1 &&
                mock.events[0] == MOCK_EVENT_VALIDATE);
         push.dwords[0] = 0;
         assert(!NvxNouveauPushbufValidate(&push));
         assert(!NvxNouveauPushbufAppend(&push, packet,
                                          NVX_3D_MAX_PUSH_DWORDS));
         NvxNouveauPushbufReset(&push);
         assert(push.dword_count == 0 && push.relocation_count == 0);
         NvxNouveauPushbufDestroy(&push);
      }

      mock.event_count = 0;
      mock.fail_wait = 1;
      assert(NvxNouveauBoMap(&bo, NVX_NOUVEAU_BO_READ) == NULL);
      assert(mock.event_count == 1 && mock.events[0] == MOCK_EVENT_WAIT);
      mock.fail_wait = 0;

      mapped = NvxNouveauBoMap(&bo, NVX_NOUVEAU_BO_READ);
      assert(mapped && mock.waited_fence == 77);
      assert(mock.event_count >= 3 &&
             mock.events[1] == MOCK_EVENT_WAIT &&
             mock.events[2] == MOCK_EVENT_DOWNLOAD);
      assert(mock.download_bytes == bytes + 4096);
      assert(NvxNouveauBoUnmap(&bo));

      mapped = NvxNouveauBoMap(&bo, NVX_NOUVEAU_BO_READ |
                                    NVX_NOUVEAU_BO_WRITE);
      assert(mapped);
      mapped[0] = 0xA5;
      assert(NvxNouveauBoUnmap(&bo));
      assert(find_buffer(&mock, bo.allocation.BufferId)->storage[0] == 0xA5);
      assert(mock.upload_bytes == bytes + 4096);

      mapped = NvxNouveauBoMap(&bo, NVX_NOUVEAU_BO_WRITE);
      assert(mapped && mapped[0] == 0xA5 && mapped[1] == 0);
      mapped[1] = 0x5A;
      assert(NvxNouveauBoUnmap(&bo));
      assert(find_buffer(&mock, bo.allocation.BufferId)->storage[0] == 0xA5 &&
             find_buffer(&mock, bo.allocation.BufferId)->storage[1] == 0x5A);
      assert(mock.download_bytes == bytes + 12288);
      assert(mock.upload_bytes == bytes + 8192);

      mapped = NvxNouveauBoMap(&bo, NVX_NOUVEAU_BO_WRITE);
      assert(mapped);
      mapped[2] = 0x3C;
      mock.event_count = 0;
      assert(NvxNouveauBoFlush(&bo));
      assert(mock.event_count == 1 && mock.events[0] == MOCK_EVENT_UPLOAD);
      assert(find_buffer(&mock, bo.allocation.BufferId)->storage[2] == 0x3C);
      assert(bo.map_count == 1 && bo.shadow == mapped);
      mapped[2] = 0xA4;
      assert(NvxNouveauBoFlush(&bo));
      assert(find_buffer(&mock, bo.allocation.BufferId)->storage[2] == 0xA4);
      assert(NvxNouveauBoUnmap(&bo));
      assert(NvxNouveauBoFlush(&bo));

      NvxNouveauBoReference(&bo);
      assert(bo.references == 2);
      assert(NvxNouveauBoDestroy(&bo));
      assert(bo.references == 1 && mock.freed_buffer == 2);
      mock.event_count = 0;
      mock.fail_wait = 1;
      assert(!NvxNouveauBoDestroy(&bo));
      assert(bo.references == 1 && mock.freed_buffer == 2);
      assert(mock.event_count == 1 && mock.events[0] == MOCK_EVENT_WAIT);
      mock.fail_wait = 0;
      mock.event_count = 0;
      assert(NvxNouveauBoDestroy(&bo));
      assert(mock.freed_buffer == 3);
      assert(mock.event_count == 2 &&
             mock.events[0] == MOCK_EVENT_WAIT &&
             mock.events[1] == MOCK_EVENT_FREE);
   }

   {
      NVX_3D_ALLOC_BUFFER description = {0};
      NVX_NOUVEAU_BO first, second;
      uint8_t *mapped;
      struct mock_buffer *first_storage, *second_storage;

      description.Usage = 1;
      description.Bytes = 4096;
      assert(NvxNouveauBoCreate(&device, &description,
                                0x123400000003ULL, &first));
      description.Bytes = 8192;
      assert(NvxNouveauBoCreate(&device, &description,
                                0x123400000004ULL, &second));
      assert(first.allocation.BufferId != second.allocation.BufferId);
      assert(first.allocation.GpuVa != second.allocation.GpuVa);

      mapped = NvxNouveauBoMap(&first, NVX_NOUVEAU_BO_WRITE);
      assert(mapped);
      mapped[0] = 0x31;
      assert(NvxNouveauBoUnmap(&first));
      mapped = NvxNouveauBoMap(&second, NVX_NOUVEAU_BO_WRITE);
      assert(mapped);
      mapped[0] = 0x72;
      assert(NvxNouveauBoUnmap(&second));

      first_storage = find_buffer(&mock, first.allocation.BufferId);
      second_storage = find_buffer(&mock, second.allocation.BufferId);
      assert(first_storage && second_storage);
      assert(first_storage->storage[0] == 0x31);
      assert(second_storage->storage[0] == 0x72);
      assert(first_storage->bytes == 4096 && second_storage->bytes == 8192);

      assert(NvxNouveauBoMarkUsed(&first, 80));
      assert(NvxNouveauBoMarkUsed(&second, 81));
      mock.event_count = 0;
      assert(NvxNouveauBoDestroy(&first));
      assert(mock.event_count == 2 &&
             mock.events[0] == MOCK_EVENT_WAIT &&
             mock.events[1] == MOCK_EVENT_FREE);
      assert(find_buffer(&mock, second.allocation.BufferId) != NULL);
      mock.event_count = 0;
      assert(NvxNouveauBoDestroy(&second));
      assert(mock.event_count == 2 &&
             mock.events[0] == MOCK_EVENT_WAIT &&
             mock.events[1] == MOCK_EVENT_FREE);
   }

   {
      NVX_3D_ALLOC_BUFFER description = {0};
      NVX_3D_COLOR_SURFACE_LAYOUT layout;
      NVX_NOUVEAU_BO target;
      uint64_t submitted_fence = 0;
      uint32_t d[ NVX_3D_NVC0_CLEAR_DWORDS ] = {0};
      uint32_t color_bits[4] = {0x3f000000U, 0x3e800000U,
                                0x3f800000U, 0x3f800000U};
      uint32_t i;

      assert(Nvx3dComputeColorSurfaceLayout(64, 16, &layout));
      description.Usage = NVX_3D_USAGE_COLOR_TARGET;
      description.Bytes = layout.Bytes;
      description.Width = layout.Width;
      description.Height = layout.Height;
      description.PitchBytes = layout.PitchBytes;
      description.TileMode = layout.TileMode;
      assert(NvxNouveauBoCreate(&device, &description,
                                0x123400000005ULL, &target));
      target.device = &nouveau_device.winsys;
      d[0] = Nvx3dNvc0SeqHeader(0x0d80U, 4U);
      for (i = 0; i < 4; i++) d[1 + i] = color_bits[i];
      d[5] = Nvx3dNvc0SeqHeader(0x0ff4U, 2U);
      d[6] = layout.Width << 16; d[7] = layout.Height << 16;
      d[8] = Nvx3dNvc0SeqHeader(0x0d6cU, 2U);
      d[9] = layout.Width << 16; d[10] = layout.Height << 16;
      d[11] = Nvx3dNvc0SeqHeader(0x10f8U, 1U); d[12] = 0x10U;
      d[13] = Nvx3dNvc0SeqHeader(0x121cU, 1U); d[14] = 1U;
      d[15] = Nvx3dNvc0SeqHeader(0x0800U, 9U);
      d[16] = (uint32_t)(target.allocation.GpuVa >> 32);
      d[17] = (uint32_t)target.allocation.GpuVa;
      d[18] = layout.Width; d[19] = layout.Height;
      d[20] = 0xcfU; d[21] = layout.TileMode; d[22] = 1U;
      d[23] = layout.Bytes / 4U;
      d[25] = Nvx3dNvc0ImmediateHeader(0x15d0U, 0U);
      d[26] = Nvx3dNvc0NonIncHeader(0x19d0U, 1U); d[27] = 0x3cU;
      assert(NvxNouveauScreenInit(&nouveau_screen, &nouveau_device,
                                  target.allocation.ClientToken));
      mock.event_count = 0;
      assert(NvxNouveauScreenSubmitNvc0Clear(&nouveau_screen, &target, d,
                                             NVX_3D_NVC0_CLEAR_DWORDS,
                                             &submitted_fence));
      assert(submitted_fence == mock.submitted_fence);
      assert(target.last_fence == mock.submitted_fence);
      assert(mock.event_count == 2 &&
             mock.events[0] == MOCK_EVENT_VALIDATE &&
             mock.events[1] == MOCK_EVENT_SUBMIT);
      d[0] = 0;
      mock.event_count = 0;
      assert(!NvxNouveauScreenSubmitNvc0Clear(&nouveau_screen, &target, d,
                                              NVX_3D_NVC0_CLEAR_DWORDS,
                                              &submitted_fence));
      assert(submitted_fence == 0);
      assert(mock.event_count == 1 &&
             mock.events[0] == MOCK_EVENT_VALIDATE);
      NvxNouveauScreenDestroy(&nouveau_screen);
      assert(NvxNouveauBoDestroy(&target));
   }

   assert(NvxWinsysCloseClient(&device));
   assert(device.ClientToken == 0 && mock.client_token == 0);
   {
      uint32_t i;
      for (i = 0; i < MOCK_MAX_BUFFERS; i++)
         free(mock.buffers[i].storage);
   }

   return 0;
}
