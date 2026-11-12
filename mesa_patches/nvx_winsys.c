#include "nvx_winsys.h"

#include <stdlib.h>
#include <string.h>

static int
nvx_winsys_ready(const NVX_WINSYS_DEVICE *device)
{
   return device && device->escape && device->opaque;
}

int
NvxWinsysInit(NVX_WINSYS_DEVICE *device, NVX_WINSYS_ESCAPE escape,
              void *opaque)
{
   if (!device || !escape || !opaque)
      return 0;

   memset(device, 0, sizeof(*device));
   device->escape = escape;
   device->opaque = opaque;
   device->caps.Size = sizeof(device->caps);
   device->caps.Version = NVX_3D_ABI_VERSION;
   if (!escape(opaque, NVX_ESCAPE_3D_QUERY_CAPS, 0, NULL,
               sizeof(device->caps), &device->caps) ||
       device->caps.Size != sizeof(device->caps) ||
       device->caps.Version != NVX_3D_ABI_VERSION ||
       !(device->caps.Features & NVX_3D_FEATURE_USER_ALLOC) ||
       !(device->caps.Features & NVX_3D_FEATURE_UPLOAD)) {
      memset(device, 0, sizeof(*device));
      return 0;
   }
   {
      NVX_3D_CLIENT_SESSION session;
      memset(&session, 0, sizeof(session));
      session.Size = sizeof(session);
      session.Version = NVX_3D_ABI_VERSION;
      session.Operation = NVX_3D_CLIENT_OPEN;
      if (!escape(opaque, NVX_ESCAPE_3D_CLIENT_OPEN, sizeof(session),
                  &session, sizeof(session), &session) ||
          !Nvx3dValidateClientSession(&session, NVX_3D_CLIENT_OPEN) ||
          !session.ClientToken) {
         memset(device, 0, sizeof(*device));
         return 0;
      }
      device->ClientToken = session.ClientToken;
   }
   return 1;
}

int
NvxWinsysCloseClient(NVX_WINSYS_DEVICE *device)
{
   NVX_3D_CLIENT_SESSION session;
   int ok;
   if (!nvx_winsys_ready(device) || !device->ClientToken)
      return 0;
   memset(&session, 0, sizeof(session));
   session.Size = sizeof(session);
   session.Version = NVX_3D_ABI_VERSION;
   session.Operation = NVX_3D_CLIENT_CLOSE;
   session.ClientToken = device->ClientToken;
   ok = device->escape(device->opaque, NVX_ESCAPE_3D_CLIENT_CLOSE,
                       sizeof(session), &session, 0, NULL);
   if (ok)
      device->ClientToken = 0;
   return ok;
}

uint64_t
NvxWinsysNextRequestToken(NVX_WINSYS_DEVICE *device)
{
   if (!device || !device->ClientToken)
      return 0;
   if (++device->NextRequestToken == 0)
      ++device->NextRequestToken;
   return device->NextRequestToken;
}

int
NvxWinsysAllocBuffer(NVX_WINSYS_DEVICE *device,
                     const NVX_3D_ALLOC_BUFFER *request,
                     NVX_3D_ALLOC_BUFFER *result)
{
   NVX_3D_ALLOC_BUFFER input;
   NVX_3D_ALLOC_BUFFER output;

   if (!nvx_winsys_ready(device) || !request || !result ||
       !Nvx3dValidateAlloc(request) || !device->ClientToken)
      return 0;

   input = *request;
   input.ClientToken = device->ClientToken;
   if (++device->NextAllocationToken == 0)
      ++device->NextAllocationToken;
   input.AllocationToken = device->NextAllocationToken;
   memset(&output, 0, sizeof(output));
   if (!device->escape(device->opaque, NVX_ESCAPE_3D_ALLOC,
                       sizeof(input), &input, sizeof(output), &output) ||
       output.Size != sizeof(output) ||
       output.Version != NVX_3D_ABI_VERSION ||
       output.BufferId == 0 || output.GpuVa == 0 ||
       output.Bytes != request->Bytes || output.Usage != request->Usage ||
       output.Width != request->Width || output.Height != request->Height ||
       output.PitchBytes != request->PitchBytes ||
       output.TileMode != request->TileMode ||
       output.ClientToken != input.ClientToken ||
       output.AllocationToken != input.AllocationToken)
      return 0;

   *result = output;
   return 1;
}

int
NvxWinsysFreeBuffer(NVX_WINSYS_DEVICE *device, uint64_t buffer_id)
{
   NVX_3D_FREE_BUFFER request;

   if (!nvx_winsys_ready(device) || !buffer_id)
      return 0;
   memset(&request, 0, sizeof(request));
   request.Size = sizeof(request);
   request.Version = NVX_3D_ABI_VERSION;
   request.BufferId = buffer_id;
   request.ClientToken = device->ClientToken;
   if (!Nvx3dValidateFree(&request))
      return 0;
   return device->escape(device->opaque, NVX_ESCAPE_3D_FREE,
                         sizeof(request), &request, 0, NULL);
}

int
NvxWinsysUploadBuffer(NVX_WINSYS_DEVICE *device, uint64_t buffer_id,
                      uint32_t allocated_bytes, uint32_t offset,
                      const void *data, uint32_t bytes)
{
   NVX_3D_UPLOAD_BUFFER *request;
   uint32_t remaining = bytes;
   uint32_t current_offset = offset;
   uint32_t capacity;
   const uint8_t *source = (const uint8_t *)data;
   int ok = 0;

   if (!nvx_winsys_ready(device) || !buffer_id || !data || !bytes ||
       (offset & 3U) || (bytes & 3U) || offset > allocated_bytes ||
       bytes > allocated_bytes - offset)
      return 0;

   capacity = bytes < NVX_3D_MAX_UPLOAD_BYTES ?
      bytes : NVX_3D_MAX_UPLOAD_BYTES;
   request = (NVX_3D_UPLOAD_BUFFER *)malloc(sizeof(*request) + capacity);
   if (!request)
      return 0;

   while (remaining) {
      uint32_t chunk = remaining;
      uint32_t input_bytes;
      if (chunk > NVX_3D_MAX_UPLOAD_BYTES)
         chunk = NVX_3D_MAX_UPLOAD_BYTES;
      chunk &= ~3U;
      if (!chunk)
         goto done;

      memset(request, 0, sizeof(*request));
      request->Size = sizeof(*request);
      request->Version = NVX_3D_ABI_VERSION;
      request->BufferId = buffer_id;
      request->ClientToken = device->ClientToken;
      request->Offset = current_offset;
      request->Bytes = chunk;
      memcpy((uint8_t *)request + sizeof(*request), source, chunk);
      input_bytes = sizeof(*request) + chunk;
      if (!Nvx3dValidateUpload(request, input_bytes, allocated_bytes) ||
          !device->escape(device->opaque, NVX_ESCAPE_3D_UPLOAD,
                          input_bytes, request, 0, NULL))
         goto done;
      current_offset += chunk;
      source += chunk;
      remaining -= chunk;
   }
   ok = 1;

done:
   free(request);
   return ok;
}

int
NvxWinsysDownloadBuffer(NVX_WINSYS_DEVICE *device, uint64_t buffer_id,
                        uint32_t allocated_bytes, uint32_t offset,
                        void *data, uint32_t bytes)
{
   NVX_3D_DOWNLOAD_BUFFER request;
   uint8_t *destination = (uint8_t *)data;
   uint32_t remaining = bytes;
   uint32_t current_offset = offset;

   if (!nvx_winsys_ready(device) ||
       !(device->caps.Features & NVX_3D_FEATURE_DOWNLOAD) ||
       !buffer_id || !data || !bytes || (offset & 3U) || (bytes & 3U) ||
       offset > allocated_bytes || bytes > allocated_bytes - offset)
      return 0;

   while (remaining) {
      uint32_t chunk = remaining;
      if (chunk > NVX_3D_MAX_DOWNLOAD_BYTES)
         chunk = NVX_3D_MAX_DOWNLOAD_BYTES;
      chunk &= ~3U;
      if (!chunk)
         return 0;

      memset(&request, 0, sizeof(request));
      request.Size = sizeof(request);
      request.Version = NVX_3D_ABI_VERSION;
      request.BufferId = buffer_id;
      request.ClientToken = device->ClientToken;
      request.Offset = current_offset;
      request.Bytes = chunk;
      if (!Nvx3dValidateDownload(&request, sizeof(request), chunk,
                                 allocated_bytes) ||
          !device->escape(device->opaque, NVX_ESCAPE_3D_DOWNLOAD,
                          sizeof(request), &request, chunk, destination))
         return 0;
      current_offset += chunk;
      destination += chunk;
      remaining -= chunk;
   }
   return 1;
}

int
NvxWinsysWaitFence(NVX_WINSYS_DEVICE *device, uint64_t fence_value,
                   uint32_t timeout_ms)
{
   NVX_3D_WAIT_FENCE request;
   NVX_3D_WAIT_FENCE result;

   if (!nvx_winsys_ready(device) || !fence_value || !timeout_ms)
      return 0;
   memset(&request, 0, sizeof(request));
   request.Size = sizeof(request);
   request.Version = NVX_3D_ABI_VERSION;
   request.FenceValue = fence_value;
   request.TimeoutMs = timeout_ms;
   memset(&result, 0, sizeof(result));
   return device->escape(device->opaque, NVX_ESCAPE_3D_WAIT,
                         sizeof(request), &request, sizeof(result), &result) &&
          result.Size == sizeof(result) &&
          result.Version == NVX_3D_ABI_VERSION &&
          result.Status == NVX_3D_STATUS_SUCCESS &&
          result.FenceValue == fence_value;
}

int
NvxWinsysValidateBatch(NVX_WINSYS_DEVICE *device,
                       const void *batch, uint32_t batch_bytes)
{
   NVX_3D_BATCH_HEADER header;
   uint32_t required_bytes;

   if (!nvx_winsys_ready(device) || !batch ||
       batch_bytes < sizeof(header) ||
       batch_bytes > NVX_3D_MAX_BATCH_BYTES)
      return 0;

   memcpy(&header, batch, sizeof(header));
   if (!Nvx3dBatchRequiredBytes(&header, &required_bytes) ||
       required_bytes != batch_bytes)
      return 0;

   /* The kernel copies and validates this batch. This escape does not submit
    * commands and must not be treated as a pushbuffer kick. */
   return device->escape(device->opaque, NVX_ESCAPE_3D_VALIDATE_BATCH,
                         batch_bytes, batch, 0, NULL);
}
