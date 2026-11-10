#ifndef NVX_3D_ABI_H
#define NVX_3D_ABI_H

#include <stdint.h>
#include <stddef.h>

/* Fixed-width ABI shared by XP x64/x86 clients and the miniport.  GPU VAs are
 * always 64-bit; no kernel pointers or RM handles cross this boundary. */
#define NVX_3D_ABI_VERSION 5U
#define NVX_3D_MAX_PUSH_DWORDS 4096U
#define NVX_3D_PAGE_BYTES 4096U
#define NVX_3D_MAX_BUFFER_BYTES (64U * 1024U * 1024U)
#define NVX_3D_MAX_UPLOAD_BYTES 0x0000F000U
#define NVX_3D_TRIANGLE_TARGET_BYTES 65536U
#define NVX_3D_TRIANGLE_WIDTH 64U
#define NVX_3D_TRIANGLE_HEIGHT 16U
#define NVX_3D_MAX_SURFACE_DIMENSION 4096U
#define NVX_3D_GOB_WIDTH_BYTES 64U
#define NVX_3D_GOB_HEIGHT_ROWS 8U
#define NVX_3D_SURFACE_ALIGNMENT 65536U
#define NVX_3D_READBACK_PIXELS (NVX_3D_TRIANGLE_WIDTH * NVX_3D_TRIANGLE_HEIGHT)
#define NVX_3D_READBACK_BYTES (NVX_3D_READBACK_PIXELS * sizeof(uint32_t))

#define NVX_3D_FEATURE_GR_CHANNEL       0x00000001U
#define NVX_3D_FEATURE_I2M_VERIFIED     0x00000002U
#define NVX_3D_FEATURE_CLEAR_VERIFIED   0x00000004U
#define NVX_3D_FEATURE_SUBMIT           0x00000008U
#define NVX_3D_FEATURE_USER_ALLOC       0x00000010U
#define NVX_3D_FEATURE_TEST_TRIANGLE    0x00000020U
#define NVX_3D_FEATURE_UPLOAD           0x00000040U
#define NVX_3D_FEATURE_CLIENT_TRIANGLE   0x00000080U

#define NVX_3D_OP_QUERY_CAPS  1U
#define NVX_3D_OP_ALLOC       2U
#define NVX_3D_OP_SUBMIT      3U
#define NVX_3D_OP_WAIT        4U
#define NVX_3D_OP_FREE        5U
#define NVX_3D_OP_READBACK    6U
#define NVX_3D_SUBMIT_CLEAR_SURFACE 1U
#define NVX_3D_SUBMIT_TEST_TRIANGLE 2U
#define NVX_3D_SUBMIT_DRAW_TRIANGLE 3U
#define NVX_3D_SUBMIT_DRAW_TRIANGLE_PRESERVE 4U
#define NVX_3D_SUBMIT_CLEAR_DEPTH 5U
#define NVX_3D_SUBMIT_DRAW_TRIANGLE_DEPTH 6U
#define NVX_3D_SUBMIT_DRAW_TRIANGLES 7U
#define NVX_3D_SUBMIT_CLEAR_COLOR_TARGET 8U
#define NVX_3D_MAX_BATCH_VERTICES 36U
#define NVX_3D_USAGE_DEPTH_TARGET 2U
#define NVX_3D_USAGE_DEPTH_S8Z24 4U
#define NVX_3D_USAGE_DEPTH_METHOD_S8Z24 8U
#define NVX_3D_USAGE_DEPTH_ZF32 16U
#define NVX_3D_USAGE_COLOR_TARGET 32U
#define NVX_3D_ZT_FORMAT_Z24S8 0U
#define NVX_3D_ZT_FORMAT_X8Z24 1U
#define NVX_3D_ZT_FORMAT_S8Z24 2U
#define NVX_3D_ZT_FORMAT_ZF32 3U
#define NVX_3D_DEPTH_TARGET_OFFSET NVX_3D_TRIANGLE_TARGET_BYTES
#define NVX_3D_DEPTH_TARGET_BYTES 65536U
#define NVX_3D_DEPTH_TARGET_TOTAL_BYTES \
    (NVX_3D_DEPTH_TARGET_OFFSET + NVX_3D_DEPTH_TARGET_BYTES)
#define NVX_ESCAPE_3D_QUERY_CAPS  0x8001U
#define NVX_ESCAPE_3D_ALLOC       0x8002U
#define NVX_ESCAPE_3D_SUBMIT      0x8003U
#define NVX_ESCAPE_3D_WAIT        0x8004U
#define NVX_ESCAPE_3D_FREE        0x8005U
#define NVX_ESCAPE_3D_UPLOAD      0x8006U
#define NVX_ESCAPE_3D_READBACK    0x8007U
#define NVX_3D_STATUS_SUCCESS            0U
#define NVX_3D_STATUS_INVALID_PARAMETER 87U
#define NVX_3D_STATUS_NOT_SUPPORTED     50U
#define NVX_3D_STATUS_BUFFER_TOO_SMALL 122U

/* Checked block-linear RGBA8 layout for a 2D C797 color target. This mirrors
 * the NVC0/NIL 64x8-byte GOB tiling and height-based block-height selection;
 * allocation remains 64 KiB aligned for the current VRAM page-table mapping.
 * TileMode is the NVC797_SET_COLOR_TARGET_MEMORY encoding (Y in bits 6:4). */
typedef struct NVX_3D_COLOR_SURFACE_LAYOUT {
    uint32_t Width;
    uint32_t Height;
    uint32_t PitchBytes;
    uint32_t TileMode;
    uint32_t Bytes;
    uint32_t Reserved;
} NVX_3D_COLOR_SURFACE_LAYOUT;

static inline uint32_t
Nvx3dAlignU32(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static inline int
Nvx3dComputeColorSurfaceLayout(uint32_t width, uint32_t height,
                               NVX_3D_COLOR_SURFACE_LAYOUT *layout)
{
    uint32_t yLog2, blockRows, alignedHeight, pitch, bytes;
    uint64_t rawBytes;

    if (!layout || !width || !height ||
        width > NVX_3D_MAX_SURFACE_DIMENSION ||
        height > NVX_3D_MAX_SURFACE_DIMENSION)
        return 0;

    /* NIL's proprietary-driver heuristic for 2D image height. */
    yLog2 = height >= 86U ? 4U : height >= 43U ? 3U :
            height >= 22U ? 2U : height >= 11U ? 1U : 0U;
    blockRows = (height + NVX_3D_GOB_HEIGHT_ROWS - 1U) /
                NVX_3D_GOB_HEIGHT_ROWS;
    while(yLog2 && (1U << yLog2) > blockRows) yLog2--;
    while(yLog2 && (1U<<yLog2)>blockRows) yLog2--;

    if ((uint64_t)width * 4U > 0xffffffffULL)
        return 0;
    pitch = Nvx3dAlignU32(width * 4U, NVX_3D_GOB_WIDTH_BYTES);
    alignedHeight = Nvx3dAlignU32(height,
                                  NVX_3D_GOB_HEIGHT_ROWS << yLog2);
    rawBytes = (uint64_t)pitch * alignedHeight;
    if (rawBytes > NVX_3D_MAX_BUFFER_BYTES)
        return 0;
    bytes = Nvx3dAlignU32((uint32_t)rawBytes, NVX_3D_SURFACE_ALIGNMENT);
    if (bytes > NVX_3D_MAX_BUFFER_BYTES)
        return 0;

    layout->Width = width;
    layout->Height = height;
    layout->PitchBytes = pitch;
    layout->TileMode = yLog2 << 4;
    layout->Bytes = bytes;
    layout->Reserved = 0;
    return 1;
}

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
    uint32_t Width;
    uint32_t Height;
    uint32_t PitchBytes;
    uint32_t TileMode;
} NVX_3D_ALLOC_BUFFER;

typedef struct NVX_3D_FREE_BUFFER {
    uint32_t Size;
    uint32_t Version;
    uint64_t BufferId;
} NVX_3D_FREE_BUFFER;

/* Follow this fixed header with Bytes payload bytes. Uploads are chunked so
 * the copy engine can stage them through its bounded coherent DMA window. */
typedef struct NVX_3D_UPLOAD_BUFFER {
    uint32_t Size;
    uint32_t Version;
    uint64_t BufferId;
    uint32_t Offset;
    uint32_t Bytes;
    uint32_t Reserved[2];
} NVX_3D_UPLOAD_BUFFER;

typedef struct NVX_3D_SUBMIT {
    uint32_t Size;
    uint32_t Version;
    uint32_t Operation;
    uint32_t Width;
    /* Fixed target height is 16 except DRAW_TRIANGLES, where Height is the
     * vertex count (3..36, divisible by three). */
    uint32_t Height;
    /* CLEAR_SURFACE and draw operations use packed AARRGGBB color. */
    uint32_t Color;
    uint64_t BufferId;
    uint64_t FenceValue;
    /* DRAW_TRIANGLE input: low/high halves of VertexBufferId. On output the
       fields report drawn and background pixel counts, respectively. */
    uint32_t Reserved[2];
} NVX_3D_SUBMIT;

typedef struct NVX_3D_WAIT_FENCE {
    uint32_t Size;
    uint32_t Version;
    uint64_t FenceValue;
    uint32_t TimeoutMs;
    uint32_t Status;
} NVX_3D_WAIT_FENCE;

/* Readback is restricted to the verified 64x16 block-linear C797 target.
 * Output is a row-major array of 32-bit AARRGGBB pixels. */
typedef struct NVX_3D_READBACK {
    uint32_t Size;
    uint32_t Version;
    uint64_t BufferId;
    uint32_t Width;
    uint32_t Height;
    uint32_t Reserved[2];
} NVX_3D_READBACK;

static inline int
Nvx3dValidateReadback(const NVX_3D_READBACK *request, uint32_t outputBytes,
                      uint32_t allocatedBytes)
{
    return request && request->Size==sizeof(*request) &&
        request->Version==NVX_3D_ABI_VERSION && request->BufferId &&
        request->Width==NVX_3D_TRIANGLE_WIDTH &&
        request->Height==NVX_3D_TRIANGLE_HEIGHT &&
        outputBytes>=NVX_3D_READBACK_BYTES &&
        allocatedBytes>=NVX_3D_TRIANGLE_TARGET_BYTES &&
        !request->Reserved[0] && !request->Reserved[1];
}

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
    if(userAllocReady) caps->Features|=NVX_3D_FEATURE_USER_ALLOC|
        NVX_3D_FEATURE_UPLOAD;
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
    uint64_t vertexBufferId;
    int validOperation;
    if(!request || inputBytes!=sizeof(*request) || request->Size!=sizeof(*request) ||
       request->Version!=NVX_3D_ABI_VERSION ||
       request->BufferId==0 ||
       request->FenceValue || allocatedBytes<4096) return 0;
    validOperation=request->Operation==NVX_3D_SUBMIT_CLEAR_SURFACE ||
        request->Operation==NVX_3D_SUBMIT_TEST_TRIANGLE ||
        request->Operation==NVX_3D_SUBMIT_DRAW_TRIANGLE ||
        request->Operation==NVX_3D_SUBMIT_DRAW_TRIANGLE_PRESERVE ||
        request->Operation==NVX_3D_SUBMIT_CLEAR_DEPTH ||
        request->Operation==NVX_3D_SUBMIT_DRAW_TRIANGLE_DEPTH ||
        request->Operation==NVX_3D_SUBMIT_DRAW_TRIANGLES;
    if(!validOperation &&
       request->Operation!=NVX_3D_SUBMIT_CLEAR_COLOR_TARGET) return 0;
    if(request->Operation==NVX_3D_SUBMIT_CLEAR_COLOR_TARGET) {
        if(!request->Width || !request->Height ||
           request->Width>NVX_3D_MAX_SURFACE_DIMENSION ||
           request->Height>NVX_3D_MAX_SURFACE_DIMENSION ||
           request->Reserved[0] || request->Reserved[1]) return 0;
        return 1;
    }
    if(request->Width!=64) return 0;
    if(request->Operation==NVX_3D_SUBMIT_DRAW_TRIANGLES) {
        if(request->Height<3U || request->Height>NVX_3D_MAX_BATCH_VERTICES ||
           request->Height%3U) return 0;
    } else if(request->Height!=16U) return 0;
    if(request->Color &&
       request->Operation!=NVX_3D_SUBMIT_CLEAR_SURFACE &&
       request->Operation!=NVX_3D_SUBMIT_DRAW_TRIANGLE &&
       request->Operation!=NVX_3D_SUBMIT_DRAW_TRIANGLE_PRESERVE &&
       request->Operation!=NVX_3D_SUBMIT_DRAW_TRIANGLE_DEPTH &&
       request->Operation!=NVX_3D_SUBMIT_DRAW_TRIANGLES) return 0;
    if((request->Operation==NVX_3D_SUBMIT_TEST_TRIANGLE ||
        request->Operation==NVX_3D_SUBMIT_DRAW_TRIANGLE ||
        request->Operation==NVX_3D_SUBMIT_DRAW_TRIANGLE_PRESERVE ||
        request->Operation==NVX_3D_SUBMIT_DRAW_TRIANGLE_DEPTH ||
        request->Operation==NVX_3D_SUBMIT_DRAW_TRIANGLES) &&
       allocatedBytes<NVX_3D_TRIANGLE_TARGET_BYTES) return 0;
    if((request->Operation==NVX_3D_SUBMIT_CLEAR_DEPTH ||
        request->Operation==NVX_3D_SUBMIT_DRAW_TRIANGLE_DEPTH) &&
       allocatedBytes<NVX_3D_DEPTH_TARGET_TOTAL_BYTES) return 0;
    vertexBufferId=((uint64_t)request->Reserved[1]<<32)|request->Reserved[0];
    if(request->Operation==NVX_3D_SUBMIT_DRAW_TRIANGLE ||
       request->Operation==NVX_3D_SUBMIT_DRAW_TRIANGLE_PRESERVE ||
       request->Operation==NVX_3D_SUBMIT_DRAW_TRIANGLE_DEPTH ||
       request->Operation==NVX_3D_SUBMIT_DRAW_TRIANGLES) {
        if(!vertexBufferId||vertexBufferId==request->BufferId) return 0;
    } else {
        for(i=0;i<sizeof(request->Reserved)/sizeof(request->Reserved[0]);i++)
            if(request->Reserved[i]) return 0;
        if(request->Operation==NVX_3D_SUBMIT_TEST_TRIANGLE&&request->Color)
            return 0;
    }
    return 1;
}

static inline uint64_t
Nvx3dSubmitVertexBufferId(const NVX_3D_SUBMIT *request)
{
    return request?(((uint64_t)request->Reserved[1]<<32)|
                    request->Reserved[0]):0;
}

static inline uint32_t
Nvx3dRequiredSubmitBytes(uint32_t operation)
{
    if(operation==NVX_3D_SUBMIT_CLEAR_DEPTH ||
       operation==NVX_3D_SUBMIT_DRAW_TRIANGLE_DEPTH)
        return NVX_3D_DEPTH_TARGET_TOTAL_BYTES;
    if(operation==NVX_3D_SUBMIT_TEST_TRIANGLE ||
       operation==NVX_3D_SUBMIT_DRAW_TRIANGLE ||
       operation==NVX_3D_SUBMIT_DRAW_TRIANGLE_PRESERVE ||
       operation==NVX_3D_SUBMIT_DRAW_TRIANGLES)
        return NVX_3D_TRIANGLE_TARGET_BYTES;
    return 4096U;
}

static inline int
Nvx3dValidateFree(const NVX_3D_FREE_BUFFER *request)
{
    return request && request->Size==sizeof(*request) &&
        request->Version==NVX_3D_ABI_VERSION && request->BufferId;
}

static inline int
Nvx3dValidateUpload(const NVX_3D_UPLOAD_BUFFER *request,
                    uint32_t inputBytes, uint32_t allocatedBytes)
{
    uint32_t i;
    if(!request || inputBytes<sizeof(*request) ||
       request->Size!=sizeof(*request) ||
       request->Version!=NVX_3D_ABI_VERSION || !request->BufferId ||
       !request->Bytes || request->Bytes>NVX_3D_MAX_UPLOAD_BYTES ||
       (request->Offset&3U) || (request->Bytes&3U) ||
       inputBytes!=sizeof(*request)+request->Bytes ||
       request->Offset>allocatedBytes ||
       request->Bytes>allocatedBytes-request->Offset) return 0;
    for(i=0;i<sizeof(request->Reserved)/sizeof(request->Reserved[0]);i++)
        if(request->Reserved[i]) return 0;
    return 1;
}

static inline int
Nvx3dValidateAlloc(const NVX_3D_ALLOC_BUFFER *request)
{
    NVX_3D_COLOR_SURFACE_LAYOUT layout;
    if(!request || request->Size!=sizeof(*request) ||
       request->Version!=NVX_3D_ABI_VERSION || request->Usage>63U ||
       ((request->Usage&NVX_3D_USAGE_DEPTH_S8Z24) &&
        !(request->Usage&NVX_3D_USAGE_DEPTH_TARGET)) ||
       ((request->Usage&NVX_3D_USAGE_DEPTH_METHOD_S8Z24) &&
        !(request->Usage&NVX_3D_USAGE_DEPTH_S8Z24)) ||
       ((request->Usage&NVX_3D_USAGE_DEPTH_ZF32) &&
        !(request->Usage&NVX_3D_USAGE_DEPTH_TARGET)) ||
       ((request->Usage&NVX_3D_USAGE_DEPTH_ZF32) &&
        (request->Usage&(NVX_3D_USAGE_DEPTH_S8Z24|
                         NVX_3D_USAGE_DEPTH_METHOD_S8Z24))) ||
       ((request->Usage&NVX_3D_USAGE_COLOR_TARGET) &&
        (request->Usage&NVX_3D_USAGE_DEPTH_TARGET)) ||
       !request->Bytes || request->Bytes>NVX_3D_MAX_BUFFER_BYTES ||
       (request->Bytes&(NVX_3D_PAGE_BYTES-1U)) ||
       request->BufferId || request->GpuVa)
        return 0;

    if(request->Usage&NVX_3D_USAGE_COLOR_TARGET) {
        if(!Nvx3dComputeColorSurfaceLayout(request->Width,request->Height,
                                           &layout) ||
           request->Bytes!=layout.Bytes ||
           request->PitchBytes!=layout.PitchBytes ||
           request->TileMode!=layout.TileMode)
            return 0;
    } else if(request->Width || request->Height || request->PitchBytes ||
              request->TileMode) {
        return 0;
    }

    return 1;
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
        if(!Nvx3dValidateSubmit((const NVX_3D_SUBMIT *)input,inputBytes,
             input&&inputBytes==sizeof(NVX_3D_SUBMIT)?
                 Nvx3dRequiredSubmitBytes(
                    ((const NVX_3D_SUBMIT *)input)->Operation):4096))
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
    case NVX_3D_OP_READBACK:
        if(!output || inputBytes!=sizeof(NVX_3D_READBACK) ||
           !Nvx3dValidateReadback((const NVX_3D_READBACK *)input,
                                  outputBytes,NVX_3D_TRIANGLE_TARGET_BYTES))
            return NVX_3D_STATUS_INVALID_PARAMETER;
        if(information) *information=NVX_3D_READBACK_BYTES;
        return NVX_3D_STATUS_SUCCESS;
    default:
        return NVX_3D_STATUS_INVALID_PARAMETER;
    }
}

#endif
