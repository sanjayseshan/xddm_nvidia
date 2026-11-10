#ifndef NVX_3D_ABI_H
#define NVX_3D_ABI_H

#include <stdint.h>
#include <stddef.h>

/* Fixed-width ABI shared by XP x64/x86 clients and the miniport.  GPU VAs are
 * always 64-bit; no kernel pointers or RM handles cross this boundary. */
#define NVX_3D_ABI_VERSION 2U
#define NVX_3D_MAX_PUSH_DWORDS 4096U
#define NVX_3D_PAGE_BYTES 4096U
#define NVX_3D_MAX_BUFFER_BYTES (64U * 1024U * 1024U)

#define NVX_3D_FEATURE_GR_CHANNEL       0x00000001U
#define NVX_3D_FEATURE_I2M_VERIFIED     0x00000002U
#define NVX_3D_FEATURE_CLEAR_VERIFIED   0x00000004U
#define NVX_3D_FEATURE_SUBMIT           0x00000008U
#define NVX_3D_FEATURE_USER_ALLOC       0x00000010U

#define NVX_3D_OP_QUERY_CAPS  1U
#define NVX_3D_OP_ALLOC       2U
#define NVX_3D_OP_SUBMIT      3U
#define NVX_3D_OP_WAIT        4U
#define NVX_3D_OP_FREE        5U
#define NVX_3D_SUBMIT_CLEAR_SURFACE 1U
#define NVX_ESCAPE_3D_QUERY_CAPS  0x8001U
#define NVX_ESCAPE_3D_ALLOC       0x8002U
#define NVX_ESCAPE_3D_SUBMIT      0x8003U
#define NVX_ESCAPE_3D_WAIT        0x8004U
#define NVX_ESCAPE_3D_FREE        0x8005U
#define NVX_3D_STATUS_SUCCESS            0U
#define NVX_3D_STATUS_INVALID_PARAMETER 87U
#define NVX_3D_STATUS_NOT_SUPPORTED     50U
#define NVX_3D_STATUS_BUFFER_TOO_SMALL 122U

typedef struct NVX_3D_QUERY_CAPS {
    uint32_t Size;
    uint32_t Version;
    uint32_t Features;
    uint32_t GraphicsClass;
    uint32_t MaxPushDwords;
    uint32_t PageBytes;
    uint32_t MaxBufferBytes;
    uint32_t Reserved;
} NVX_3D_QUERY_CAPS;

typedef struct NVX_3D_ALLOC_BUFFER {
    uint32_t Size;
    uint32_t Version;
    uint32_t Usage;
    uint32_t Bytes;
    uint64_t BufferId;
    uint64_t GpuVa;
} NVX_3D_ALLOC_BUFFER;

typedef struct NVX_3D_FREE_BUFFER {
    uint32_t Size;
    uint32_t Version;
    uint64_t BufferId;
} NVX_3D_FREE_BUFFER;

typedef struct NVX_3D_SUBMIT {
    uint32_t Size;
    uint32_t Version;
    uint32_t Operation;
    uint32_t Width;
    uint32_t Height;
    uint32_t Color;
    uint64_t BufferId;
    uint64_t FenceValue;
    uint32_t Reserved[2];
} NVX_3D_SUBMIT;

typedef struct NVX_3D_WAIT_FENCE {
    uint32_t Size;
    uint32_t Version;
    uint64_t FenceValue;
    uint32_t TimeoutMs;
    uint32_t Status;
} NVX_3D_WAIT_FENCE;

static inline void
Nvx3dBuildCaps(NVX_3D_QUERY_CAPS *caps, int channelReady,
               int commandVerified, int channelFaulted, int mmuFaulted,
               int dataWriteVerified, int clearVerified,
               int userAllocReady, int submitReady)
{
    uint32_t i;
    if(!caps) return;
    for(i=0;i<sizeof(*caps);i++) ((uint8_t *)caps)[i]=0;
    caps->Size=(uint32_t)sizeof(*caps);
    caps->Version=NVX_3D_ABI_VERSION;
    caps->GraphicsClass=0xC797U;
    caps->MaxPushDwords=NVX_3D_MAX_PUSH_DWORDS;
    caps->PageBytes=NVX_3D_PAGE_BYTES;
    caps->MaxBufferBytes=NVX_3D_MAX_BUFFER_BYTES;
    if(!channelReady || !commandVerified || channelFaulted || mmuFaulted) return;
    caps->Features=NVX_3D_FEATURE_GR_CHANNEL;
    if(dataWriteVerified) caps->Features|=NVX_3D_FEATURE_I2M_VERIFIED;
    if(clearVerified) caps->Features|=NVX_3D_FEATURE_CLEAR_VERIFIED;
    if(userAllocReady) caps->Features|=NVX_3D_FEATURE_USER_ALLOC;
    if(submitReady) caps->Features|=NVX_3D_FEATURE_SUBMIT;
}

static inline int
Nvx3dQueryCaps(void *output, uint32_t outputBytes, int channelReady,
               int commandVerified, int channelFaulted, int mmuFaulted,
               int dataWriteVerified, int clearVerified,
               int userAllocReady, int submitReady)
{
    if(!output || outputBytes<sizeof(NVX_3D_QUERY_CAPS)) return 0;
    Nvx3dBuildCaps((NVX_3D_QUERY_CAPS *)output,channelReady,commandVerified,
                   channelFaulted,mmuFaulted,dataWriteVerified,clearVerified,
                   userAllocReady,submitReady);
    return 1;
}

static inline int
Nvx3dValidateSubmit(const NVX_3D_SUBMIT *request, uint32_t inputBytes,
                    uint32_t allocatedBytes)
{
    uint32_t i;
    if(!request || inputBytes!=sizeof(*request) || request->Size!=sizeof(*request) ||
       request->Version!=NVX_3D_ABI_VERSION ||
       request->Operation!=NVX_3D_SUBMIT_CLEAR_SURFACE ||
       request->Width!=64 || request->Height!=16 || request->BufferId==0 ||
       request->Color || request->FenceValue || allocatedBytes<4096) return 0;
    for(i=0;i<sizeof(request->Reserved)/sizeof(request->Reserved[0]);i++)
        if(request->Reserved[i]) return 0;
    return 1;
}

static inline int
Nvx3dValidateFree(const NVX_3D_FREE_BUFFER *request)
{
    return request && request->Size==sizeof(*request) &&
        request->Version==NVX_3D_ABI_VERSION && request->BufferId;
}

static inline int
Nvx3dValidateAlloc(const NVX_3D_ALLOC_BUFFER *request)
{
    return request && request->Size==sizeof(*request) &&
        request->Version==NVX_3D_ABI_VERSION && request->Usage<=3U &&
        request->Bytes && request->Bytes<=NVX_3D_MAX_BUFFER_BYTES &&
        !(request->Bytes&(NVX_3D_PAGE_BYTES-1U)) &&
        !request->BufferId && !request->GpuVa;
}

static inline int
Nvx3dValidateWait(const NVX_3D_WAIT_FENCE *request)
{
    return request && request->Size==sizeof(*request) &&
        request->Version==NVX_3D_ABI_VERSION && request->FenceValue &&
        request->TimeoutMs<=30000U && request->Status==0;
}

/* Shared request framing policy used by the miniport and host tests. */
static inline uint32_t
Nvx3dDispatch(uint32_t operation, const void *input, uint32_t inputBytes,
              void *output, uint32_t outputBytes, uint32_t *information,
              int channelReady, int commandVerified, int channelFaulted,
              int mmuFaulted, int dataWriteVerified, int clearVerified)
{
    if(information) *information=0;
    switch(operation) {
    case NVX_3D_OP_QUERY_CAPS:
        if(!Nvx3dQueryCaps(output,outputBytes,channelReady,commandVerified,
            channelFaulted,mmuFaulted,dataWriteVerified,clearVerified,0,0))
            return NVX_3D_STATUS_BUFFER_TOO_SMALL;
        if(information) *information=(uint32_t)sizeof(NVX_3D_QUERY_CAPS);
        return NVX_3D_STATUS_SUCCESS;
    case NVX_3D_OP_ALLOC:
        if(inputBytes!=sizeof(NVX_3D_ALLOC_BUFFER) ||
           !Nvx3dValidateAlloc((const NVX_3D_ALLOC_BUFFER *)input))
            return NVX_3D_STATUS_INVALID_PARAMETER;
        return NVX_3D_STATUS_SUCCESS;
    case NVX_3D_OP_SUBMIT:
        if(!Nvx3dValidateSubmit((const NVX_3D_SUBMIT *)input,inputBytes,4096))
            return NVX_3D_STATUS_INVALID_PARAMETER;
        return NVX_3D_STATUS_SUCCESS;
    case NVX_3D_OP_WAIT:
        if(inputBytes!=sizeof(NVX_3D_WAIT_FENCE) ||
           !Nvx3dValidateWait((const NVX_3D_WAIT_FENCE *)input))
            return NVX_3D_STATUS_INVALID_PARAMETER;
        return NVX_3D_STATUS_SUCCESS;
    case NVX_3D_OP_FREE:
        if(inputBytes!=sizeof(NVX_3D_FREE_BUFFER) ||
           !Nvx3dValidateFree((const NVX_3D_FREE_BUFFER *)input))
            return NVX_3D_STATUS_INVALID_PARAMETER;
        return NVX_3D_STATUS_SUCCESS;
    default:
        return NVX_3D_STATUS_INVALID_PARAMETER;
    }
}

#endif
