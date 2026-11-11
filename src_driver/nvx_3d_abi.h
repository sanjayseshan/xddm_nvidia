#ifndef NVX_3D_ABI_H
#define NVX_3D_ABI_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Fixed-width ABI shared by XP x64/x86 clients and the miniport.  GPU VAs are
 * always 64-bit; no kernel pointers or RM handles cross this boundary. */
#define NVX_3D_ABI_VERSION 12U
#define NVX_3D_MAX_PUSH_DWORDS 4096U
#define NVX_3D_MAX_BATCH_RELOCATIONS 256U
#define NVX_3D_MAX_BATCH_BUFFERS 64U
#define NVX_3D_MAX_BATCH_BYTES \
    (sizeof(NVX_3D_BATCH_HEADER) + \
     NVX_3D_MAX_PUSH_DWORDS * sizeof(uint32_t) + \
     NVX_3D_MAX_BATCH_RELOCATIONS * sizeof(NVX_3D_RELOCATION))
#define NVX_3D_NVC0_CLEAR_DWORDS 28U
#define NVX_3D_PAGE_BYTES 4096U
#define NVX_3D_MAX_BUFFER_BYTES (64U * 1024U * 1024U)
#define NVX_3D_MAX_UPLOAD_BYTES 0x0000F000U
#define NVX_3D_MAX_DOWNLOAD_BYTES 0x00001000U
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
#define NVX_3D_FEATURE_DOWNLOAD          0x00000100U

#define NVX_3D_OP_QUERY_CAPS  1U
#define NVX_3D_OP_ALLOC       2U
#define NVX_3D_OP_SUBMIT      3U
#define NVX_3D_OP_WAIT        4U
#define NVX_3D_OP_FREE        5U
#define NVX_3D_OP_READBACK    6U
#define NVX_3D_OP_DOWNLOAD    7U
#define NVX_3D_SUBMIT_CLEAR_SURFACE 1U
#define NVX_3D_SUBMIT_TEST_TRIANGLE 2U
#define NVX_3D_SUBMIT_DRAW_TRIANGLE 3U
#define NVX_3D_SUBMIT_DRAW_TRIANGLE_PRESERVE 4U
#define NVX_3D_SUBMIT_CLEAR_DEPTH 5U
#define NVX_3D_SUBMIT_DRAW_TRIANGLE_DEPTH 6U
#define NVX_3D_SUBMIT_DRAW_TRIANGLES 7U
#define NVX_3D_SUBMIT_CLEAR_COLOR_TARGET 8U
#define NVX_3D_SUBMIT_DRAW_COLOR_TARGET 9U
#define NVX_3D_SUBMIT_VERIFY_COLOR_TARGET_DRAW 10U
#define NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH 11U
#define NVX_3D_SUBMIT_CLEAR_COLOR_TARGET_DEPTH 12U
#define NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH_ATTACH 13U
#define NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH_TEST_ONLY 14U
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

static inline int
Nvx3dIsDynamicDepthDraw(uint32_t operation)
{
    return operation==NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH ||
        operation==NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH_ATTACH ||
        operation==NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH_TEST_ONLY;
}

static inline uint32_t
Nvx3dDepthModeForOperation(uint32_t operation)
{
    if(operation==NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH_ATTACH) return 2U;
    if(operation==NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH_TEST_ONLY) return 3U;
    return operation==NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH?5U:0U;
}
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
#define NVX_ESCAPE_3D_NVC0_CLEAR  0x8008U
#define NVX_ESCAPE_3D_DOWNLOAD    0x8009U
#define NVX_ESCAPE_3D_VALIDATE_BATCH 0x800aU
#define NVX_ESCAPE_3D_NVC0_NATIVE_CLEAR 0x800bU
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

typedef struct NVX_3D_DEPTH_SURFACE_LAYOUT {
    uint32_t Width;
    uint32_t Height;
    uint32_t PitchBytes;
    uint32_t Bytes;
    uint32_t Reserved[2];
} NVX_3D_DEPTH_SURFACE_LAYOUT;

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

/* Ampere zeta targets use one-GOB block height. Keep rows 64-byte aligned,
 * pad to the 8-row GOB height, and align the allocation to the driver's
 * 64-KiB VRAM PTE granularity. */
static inline int
Nvx3dComputeDepthSurfaceLayout(uint32_t width, uint32_t height,
                               NVX_3D_DEPTH_SURFACE_LAYOUT *layout)
{
    uint32_t pitch, alignedHeight, bytes;
    uint64_t rawBytes;
    if(!layout || !width || !height ||
       width>NVX_3D_MAX_SURFACE_DIMENSION ||
       height>NVX_3D_MAX_SURFACE_DIMENSION ||
       (uint64_t)width*4U>0xffffffffULL) return 0;
    pitch=Nvx3dAlignU32(width*4U,NVX_3D_GOB_WIDTH_BYTES);
    alignedHeight=Nvx3dAlignU32(height,NVX_3D_GOB_HEIGHT_ROWS);
    rawBytes=(uint64_t)pitch*alignedHeight;
    if(rawBytes>NVX_3D_MAX_BUFFER_BYTES) return 0;
    bytes=Nvx3dAlignU32((uint32_t)rawBytes,NVX_3D_SURFACE_ALIGNMENT);
    if(bytes>NVX_3D_MAX_BUFFER_BYTES-NVX_3D_DEPTH_TARGET_OFFSET) return 0;
    layout->Width=width;
    layout->Height=height;
    layout->PitchBytes=pitch;
    layout->Bytes=bytes;
    layout->Reserved[0]=layout->Reserved[1]=0;
    return 1;
}

/* Byte address for an RGBA8 texel in Ampere's TuringColor2D GOB ordering. */
static inline uint32_t
Nvx3dTuringColorPixelOffset(uint32_t x, uint32_t y, uint32_t width,
                            uint32_t tileMode)
{
    uint32_t xBytes=x*4U;
    uint32_t blockHeight=1U<<((tileMode>>4)&7U);
    uint32_t gobY=y/8U;
    uint32_t gobsPerRow=(width+15U)/16U;
    uint32_t gob=((gobY/blockHeight)*gobsPerRow+(xBytes/64U))*blockHeight+
                 (gobY%blockHeight);
    uint32_t inGobX=xBytes&63U,inGobY=y&7U;
    uint32_t inGob=(inGobX/32U)*256U+((inGobX&31U)/16U)*64U+
                   (inGobY&3U)*16U+((inGobY&4U)?128U:0U)+(inGobX&15U);
    return gob*512U+inGob;
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
    uint32_t Chipset;
    uint32_t DeviceId;
    uint64_t VramApertureBytes;
    uint64_t GartBytes;
} NVX_3D_QUERY_CAPS;

static inline uint32_t
Nvx3dChipsetFromDeviceId(uint32_t deviceId)
{
    switch (deviceId) {
    case 0x2484U: /* GA104 RTX 3070 */
    case 0x2488U: /* GA104 RTX 3070 LHR */
    case 0x249dU: /* GA104 RTX 3070 mobile */
        return 0x170U;
    default:
        return 0;
    }
}

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
    uint64_t ClientToken;
} NVX_3D_ALLOC_BUFFER;

/* Resource-relative address patch for a future validated NVC0 batch. This is
 * metadata only: the miniport must verify the buffer ID, range, access, and
 * packet-word index before resolving or submitting it. */
typedef struct NVX_3D_RELOCATION {
    uint32_t CommandWord;
    uint32_t Access;
    uint64_t BufferId;
    uint64_t BufferOffset;
    uint64_t Bytes;
    uint32_t Reserved[2];
} NVX_3D_RELOCATION;

typedef struct NVX_3D_BATCH_HEADER {
    uint32_t Size;
    uint32_t Version;
    uint32_t DwordCount;
    uint32_t RelocationCount;
    uint64_t ClientToken;
    uint32_t Reserved[2];
} NVX_3D_BATCH_HEADER;

static inline int
Nvx3dBatchRequiredBytes(const NVX_3D_BATCH_HEADER *header,
                        uint32_t *requiredBytes)
{
    uint64_t required;
    if(!header||!requiredBytes||header->Size!=sizeof(*header)||
       header->Version!=NVX_3D_ABI_VERSION||!header->DwordCount||
       header->DwordCount>NVX_3D_MAX_PUSH_DWORDS||
       header->RelocationCount>NVX_3D_MAX_BATCH_RELOCATIONS)
        return 0;
    required=(uint64_t)sizeof(*header)+
             (uint64_t)header->DwordCount*sizeof(uint32_t)+
             (uint64_t)header->RelocationCount*sizeof(NVX_3D_RELOCATION);
    if(required>NVX_3D_MAX_BATCH_BYTES||required>0xffffffffULL)
        return 0;
    *requiredBytes=(uint32_t)required;
    return 1;
}

#define NVX_3D_RELOC_READ  0x01U
#define NVX_3D_RELOC_WRITE 0x02U

static inline int
Nvx3dValidateRelocation(const NVX_3D_RELOCATION *relocation,
                        uint32_t commandDwords,
                        const NVX_3D_ALLOC_BUFFER *allocation)
{
    uint32_t i;
    uint64_t address;

    if(!relocation || !allocation || !commandDwords ||
       relocation->CommandWord>=commandDwords ||
       !(relocation->Access&(NVX_3D_RELOC_READ|NVX_3D_RELOC_WRITE)) ||
       (relocation->Access&~(NVX_3D_RELOC_READ|NVX_3D_RELOC_WRITE)) ||
       !relocation->BufferId || relocation->BufferId!=allocation->BufferId ||
       !relocation->Bytes || !allocation->GpuVa ||
       relocation->BufferOffset>allocation->Bytes ||
       relocation->Bytes>(uint64_t)allocation->Bytes-
                         relocation->BufferOffset ||
       allocation->GpuVa>UINT64_MAX-relocation->BufferOffset)
        return 0;

    address=allocation->GpuVa+relocation->BufferOffset;
    if(address>UINT64_MAX-relocation->Bytes)
        return 0;
    for(i=0;i<sizeof(relocation->Reserved)/sizeof(relocation->Reserved[0]);i++)
        if(relocation->Reserved[i]) return 0;
    return 1;
}

static inline uint32_t
Nvx3dBatchReadDword(const uint8_t *dwords, uint32_t index)
{
    uint32_t value;
    memcpy(&value, dwords + (uint64_t)index * sizeof(value), sizeof(value));
    return value;
}

static inline int
Nvx3dBatchWordIsPayload(const uint8_t *dwords, uint32_t dwordCount,
                        uint32_t targetWord)
{
    uint32_t cursor=0;
    while(cursor<dwordCount) {
        uint32_t header=Nvx3dBatchReadDword(dwords,cursor);
        uint32_t packetType=header&0xe0000000U;
        if(packetType==0x80000000U)
            cursor++;
        else if(packetType==0x20000000U||packetType==0x60000000U) {
            uint32_t count=(header>>16)&0x1fffU;
            if(!count||count>dwordCount-cursor-1U) return 0;
            if(targetWord>cursor&&targetWord<=cursor+count) return 1;
            cursor+=count+1U;
        } else return 0;
    }
    return 0;
}

/* Validate a copied, bounded NVC0 batch envelope before any consumer parses
 * methods or executes work. This checks packet framing and resource-relative
 * relocations only; a kernel execution path must additionally enforce its
 * C797 method/subchannel allowlist and address-method relocation rules. */
static inline int
Nvx3dValidateNvc0Batch(const void *input, uint32_t inputBytes,
                       const NVX_3D_ALLOC_BUFFER *ownedAllocations,
                       uint32_t ownedAllocationCount,
                       uint64_t expectedClientToken)
{
    NVX_3D_BATCH_HEADER header;
    const uint8_t *bytes=(const uint8_t *)input;
    const uint8_t *dwords,*relocations;
    uint32_t requiredBytes;
    uint32_t cursor=0,i,j;

    if(!input||inputBytes<sizeof(header)||
       ownedAllocationCount>NVX_3D_MAX_BATCH_BUFFERS||
       (ownedAllocationCount&&!ownedAllocations)||!expectedClientToken)
        return 0;
    memcpy(&header,input,sizeof(header));
    if(header.Size!=sizeof(header)||header.Version!=NVX_3D_ABI_VERSION||
       !header.DwordCount||header.DwordCount>NVX_3D_MAX_PUSH_DWORDS||
       header.RelocationCount>NVX_3D_MAX_BATCH_RELOCATIONS||
       header.ClientToken!=expectedClientToken||
       header.Reserved[0]||header.Reserved[1])
        return 0;

    if(!Nvx3dBatchRequiredBytes(&header,&requiredBytes)||
       requiredBytes!=inputBytes) return 0;
    dwords=bytes+sizeof(header);
    relocations=dwords+(uint64_t)header.DwordCount*sizeof(uint32_t);

    while(cursor<header.DwordCount) {
        uint32_t packetHeader=Nvx3dBatchReadDword(dwords,cursor);
        uint32_t packetType=packetHeader&0xe0000000U;
        if(packetType==0x80000000U) {
            /* Type-8 immediate methods encode their value in the header. */
            cursor++;
        } else if(packetType==0x20000000U||packetType==0x60000000U) {
            uint32_t count=(packetHeader>>16)&0x1fffU;
            uint32_t method=packetHeader&0x1fffU;
            if(!count||count>header.DwordCount-cursor-1U)
                return 0;
            /* Incrementing packets must not roll the 13-bit method field
             * into the subchannel bits. Non-incrementing packets repeatedly
             * address one method and therefore do not advance this field. */
            if(packetType==0x20000000U&&count>0x2000U-method)
                return 0;
            cursor+=count+1U;
        } else return 0;
    }

    for(i=0;i<header.RelocationCount;i++) {
        NVX_3D_RELOCATION relocation;
        uint32_t matches=0;
        memcpy(&relocation,relocations+
               (uint64_t)i*sizeof(relocation),sizeof(relocation));
        if(!Nvx3dBatchWordIsPayload(dwords,header.DwordCount,
                                    relocation.CommandWord))
            return 0;
        for(j=0;j<i;j++) {
            NVX_3D_RELOCATION previous;
            memcpy(&previous,relocations+
                   (uint64_t)j*sizeof(previous),sizeof(previous));
            if(previous.CommandWord==relocation.CommandWord)
                return 0;
        }
        for(j=0;j<ownedAllocationCount;j++) {
            const NVX_3D_ALLOC_BUFFER *allocation=&ownedAllocations[j];
            if(allocation->BufferId!=relocation.BufferId) continue;
            if(++matches>1U||allocation->ClientToken!=header.ClientToken||
               !Nvx3dValidateRelocation(&relocation,header.DwordCount,
                                        allocation))
                return 0;
        }
        if(matches!=1U) return 0;
    }
    return 1;
}

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

/* Download one bounded chunk through the CE to miniport staging memory.
 * Offsets and lengths are dword aligned; BO mapping can combine requests. */
typedef struct NVX_3D_DOWNLOAD_BUFFER {
    uint32_t Size;
    uint32_t Version;
    uint64_t BufferId;
    uint32_t Offset;
    uint32_t Bytes;
    uint32_t Reserved[2];
} NVX_3D_DOWNLOAD_BUFFER;

typedef struct NVX_3D_SUBMIT {
    uint32_t Size;
    uint32_t Version;
    uint32_t Operation;
    uint32_t Width;
    /* Fixed target height is 16 except DRAW_TRIANGLES, where Height is the
     * vertex count (3..36), and dynamic color-target operations. */
    uint32_t Height;
    /* CLEAR_SURFACE and draw operations use packed AARRGGBB color. */
    uint32_t Color;
    uint64_t BufferId;
    uint64_t FenceValue;
    /* DRAW_TRIANGLE input: low/high halves of VertexBufferId. Dynamic color
       draw: vertex count, VBO low/high, target tile mode, and optional depth
       buffer ID low/high for DRAW_COLOR_TARGET_DEPTH. */
    uint32_t Reserved[6];
    uint64_t ClientToken;
} NVX_3D_SUBMIT;

typedef struct NVX_3D_WAIT_FENCE {
    uint32_t Size;
    uint32_t Version;
    uint64_t FenceValue;
    uint32_t TimeoutMs;
    uint32_t Status;
} NVX_3D_WAIT_FENCE;

/* Readback returns one 64x16 tile as row-major AARRGGBB pixels. For dynamic
 * color targets Reserved[0:1] specify a tile-aligned x/y origin. */
typedef struct NVX_3D_READBACK {
    uint32_t Size;
    uint32_t Version;
    uint64_t BufferId;
    uint32_t Width;
    uint32_t Height;
    uint32_t Reserved[2];
} NVX_3D_READBACK;

/* First Nouveau/NVC0 packet bridge experiment. This accepts only the exact
 * 28-dword, single-layer, full-target color clear emitted by
 * nvc0_clear_render_target; the miniport validates it and rebuilds the
 * corresponding C797 operation itself. It is intentionally not a general
 * pushbuffer ABI. */
typedef struct NVX_3D_NVC0_CLEAR {
    uint32_t Size;
    uint32_t Version;
    uint32_t DwordCount;
    uint32_t Reserved;
    uint64_t BufferId;
    uint64_t FenceValue;
    uint32_t Dwords[NVX_3D_NVC0_CLEAR_DWORDS];
    uint32_t Reserved2;
} NVX_3D_NVC0_CLEAR;

static inline uint32_t
Nvx3dNvc0SeqHeader(uint32_t method, uint32_t count)
{
    return 0x20000000U | (count << 16) | (method >> 2);
}

static inline uint32_t
Nvx3dNvc0NonIncHeader(uint32_t method, uint32_t count)
{
    return 0x60000000U | (count << 16) | (method >> 2);
}

static inline uint32_t
Nvx3dNvc0ImmediateHeader(uint32_t method, uint32_t value)
{
    return 0x80000000U | (value << 16) | (method >> 2);
}

/* Convert a positive IEEE-754 UNORM float in [0,1] to an 8-bit component,
 * round-to-nearest, using integer arithmetic suitable for the XP miniport. */
static inline uint32_t
Nvx3dNvc0Unorm8(uint32_t bits)
{
    uint32_t exponent=(bits>>23)&0xffU;
    uint64_t product;
    uint32_t shift;
    if(!exponent) return 0;
    if(exponent>=150U) return 255U;
    shift=150U-exponent;
    if(shift>32U) return 0;
    product=(uint64_t)((bits&0x7fffffU)|0x800000U)*255U;
    product+=(uint64_t)1U<<(shift-1U);
    product>>=shift;
    return product>255U?255U:(uint32_t)product;
}

static inline int
Nvx3dNvc0FloatIsUnorm(uint32_t bits)
{
    return !(bits&0x80000000U) && bits<=0x3f800000U;
}

static inline int
Nvx3dValidateNvc0Clear(const NVX_3D_NVC0_CLEAR *request,
                       uint32_t inputBytes, uint64_t expectedGpuVa,
                       uint32_t allocatedBytes, uint32_t width,
                       uint32_t height, uint32_t pitchBytes,
                       uint32_t tileMode, uint32_t *packedAarrggbb)
{
    const uint32_t *d;
    uint64_t packetGpuVa;
    NVX_3D_COLOR_SURFACE_LAYOUT layout;
    uint32_t color;
    if(!request || inputBytes!=sizeof(*request) ||
       request->Size!=sizeof(*request) ||
       request->Version!=NVX_3D_ABI_VERSION || request->Reserved ||
       request->Reserved2 || request->FenceValue || !request->BufferId ||
       request->DwordCount!=NVX_3D_NVC0_CLEAR_DWORDS ||
       !expectedGpuVa || (expectedGpuVa&0xffffULL) ||
       !Nvx3dComputeColorSurfaceLayout(width,height,&layout) ||
       layout.PitchBytes!=pitchBytes || layout.TileMode!=tileMode ||
       allocatedBytes<layout.Bytes) return 0;

    d=request->Dwords;
    if(d[0]!=Nvx3dNvc0SeqHeader(0x0d80U,4U) ||
       d[5]!=Nvx3dNvc0SeqHeader(0x0ff4U,2U) ||
       d[8]!=Nvx3dNvc0SeqHeader(0x0d6cU,2U) ||
       d[11]!=Nvx3dNvc0SeqHeader(0x10f8U,1U) || d[12]!=0x10U ||
       d[13]!=Nvx3dNvc0SeqHeader(0x121cU,1U) ||
       d[15]!=Nvx3dNvc0SeqHeader(0x0800U,9U) ||
       d[25]!=Nvx3dNvc0ImmediateHeader(0x15d0U,0U) ||
       d[26]!=Nvx3dNvc0NonIncHeader(0x19d0U,1U) || d[27]!=0x3cU)
        return 0;

    if(!Nvx3dNvc0FloatIsUnorm(d[1]) ||
       !Nvx3dNvc0FloatIsUnorm(d[2]) ||
       !Nvx3dNvc0FloatIsUnorm(d[3]) ||
       !Nvx3dNvc0FloatIsUnorm(d[4])) return 0;
    if((d[6]&0xffffU) || (d[6]>>16)!=width ||
       (d[7]&0xffffU) || (d[7]>>16)!=height || d[14]!=1U)
        return 0;
    if(d[9]!=(width<<16) || d[10]!=(height<<16))
        return 0;

    packetGpuVa=((uint64_t)d[16]<<32)|d[17];
    if(packetGpuVa!=expectedGpuVa || d[18]!=width || d[19]!=height ||
       d[20]!=0xcfU || d[21]!=(tileMode&0xffffU) || d[22]!=1U ||
       d[23]!=allocatedBytes/4U || d[24]!=0U)
        return 0;

    color=(Nvx3dNvc0Unorm8(d[4])<<24)|
          (Nvx3dNvc0Unorm8(d[1])<<16)|
          (Nvx3dNvc0Unorm8(d[2])<<8)|
           Nvx3dNvc0Unorm8(d[3]);
    if(packedAarrggbb) *packedAarrggbb=color;
    return 1;
}

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
        !(request->Reserved[0]&63U) && !(request->Reserved[1]&15U) &&
        request->Reserved[0]<NVX_3D_MAX_SURFACE_DIMENSION &&
        request->Reserved[1]<NVX_3D_MAX_SURFACE_DIMENSION;
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
    if(userAllocReady&&dataWriteVerified)
        caps->Features|=NVX_3D_FEATURE_DOWNLOAD;
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
    uint64_t vertexBufferId,depthBufferId;
    NVX_3D_COLOR_SURFACE_LAYOUT colorLayout;
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
       request->Operation!=NVX_3D_SUBMIT_CLEAR_COLOR_TARGET &&
       request->Operation!=NVX_3D_SUBMIT_DRAW_COLOR_TARGET &&
       request->Operation!=NVX_3D_SUBMIT_VERIFY_COLOR_TARGET_DRAW &&
       request->Operation!=NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH &&
       request->Operation!=NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH_ATTACH &&
       request->Operation!=NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH_TEST_ONLY &&
       request->Operation!=NVX_3D_SUBMIT_CLEAR_COLOR_TARGET_DEPTH) return 0;
    if(request->Operation==NVX_3D_SUBMIT_CLEAR_COLOR_TARGET_DEPTH) {
        if(!request->Width||!request->Height||
           request->Width>NVX_3D_MAX_SURFACE_DIMENSION||
           request->Height>NVX_3D_MAX_SURFACE_DIMENSION) return 0;
        for(i=0;i<6;i++) if(request->Reserved[i]) return 0;
        return 1;
    }
    if(request->Operation==NVX_3D_SUBMIT_CLEAR_COLOR_TARGET) {
        if(!request->Width || !request->Height ||
           request->Width>NVX_3D_MAX_SURFACE_DIMENSION ||
           request->Height>NVX_3D_MAX_SURFACE_DIMENSION ||
           request->Reserved[0] || request->Reserved[1]) return 0;
        return 1;
    }
    if(request->Operation==NVX_3D_SUBMIT_DRAW_COLOR_TARGET ||
       request->Operation==NVX_3D_SUBMIT_VERIFY_COLOR_TARGET_DRAW ||
       request->Operation==NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH ||
       request->Operation==NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH_ATTACH ||
       request->Operation==NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH_TEST_ONLY) {
        if(!Nvx3dComputeColorSurfaceLayout(request->Width,request->Height,
                                           &colorLayout) ||
           allocatedBytes<colorLayout.Bytes ||
           request->Height>NVX_3D_MAX_SURFACE_DIMENSION ||
           request->Reserved[0]<3U ||
           request->Reserved[0]>NVX_3D_MAX_BATCH_VERTICES ||
           request->Reserved[0]%3U ||
           request->Reserved[3]!=colorLayout.TileMode ||
           (!request->Reserved[1]&&!request->Reserved[2]) ||
           ((((uint64_t)request->Reserved[2]<<32)|request->Reserved[1])==
             request->BufferId))
            return 0;
        if(Nvx3dIsDynamicDepthDraw(request->Operation)) {
            depthBufferId=((uint64_t)request->Reserved[5]<<32)|
                          request->Reserved[4];
            if(!depthBufferId || depthBufferId==request->BufferId ||
               depthBufferId==(((uint64_t)request->Reserved[2]<<32)|
                                request->Reserved[1])) return 0;
        } else if(request->Reserved[4]||request->Reserved[5]) return 0;
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
    if(request&&(request->Operation==NVX_3D_SUBMIT_DRAW_COLOR_TARGET ||
                 request->Operation==NVX_3D_SUBMIT_VERIFY_COLOR_TARGET_DRAW ||
                 request->Operation==NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH ||
                 request->Operation==NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH_ATTACH ||
                 request->Operation==NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH_TEST_ONLY))
        return ((uint64_t)request->Reserved[2]<<32)|request->Reserved[1];
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
Nvx3dValidateDownload(const NVX_3D_DOWNLOAD_BUFFER *request,
                      uint32_t inputBytes, uint32_t outputBytes,
                      uint32_t allocatedBytes)
{
    uint32_t i;
    if(!request || inputBytes!=sizeof(*request) ||
       request->Size!=sizeof(*request) ||
       request->Version!=NVX_3D_ABI_VERSION || !request->BufferId ||
       !request->Bytes || request->Bytes>NVX_3D_MAX_DOWNLOAD_BYTES ||
       (request->Offset&3U) || (request->Bytes&3U) ||
       outputBytes<request->Bytes || request->Offset>allocatedBytes ||
       request->Bytes>allocatedBytes-request->Offset) return 0;
    for(i=0;i<sizeof(request->Reserved)/sizeof(request->Reserved[0]);i++)
        if(request->Reserved[i]) return 0;
    return 1;
}

static inline int
Nvx3dValidateAlloc(const NVX_3D_ALLOC_BUFFER *request)
{
    NVX_3D_COLOR_SURFACE_LAYOUT layout;
    NVX_3D_DEPTH_SURFACE_LAYOUT depthLayout;
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
    } else if(request->Usage&NVX_3D_USAGE_DEPTH_TARGET) {
        if(request->Width||request->Height||request->PitchBytes||
           request->TileMode) {
            if(!Nvx3dComputeDepthSurfaceLayout(request->Width,
                    request->Height,&depthLayout) ||
               request->PitchBytes!=depthLayout.PitchBytes ||
               request->TileMode ||
               request->Bytes!=NVX_3D_DEPTH_TARGET_OFFSET+depthLayout.Bytes)
                return 0;
        } else if(request->Bytes!=NVX_3D_DEPTH_TARGET_TOTAL_BYTES) {
            return 0;
        }
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
    case NVX_3D_OP_DOWNLOAD:
        if(!output || inputBytes!=sizeof(NVX_3D_DOWNLOAD_BUFFER) ||
           !Nvx3dValidateDownload((const NVX_3D_DOWNLOAD_BUFFER *)input,
                                  inputBytes,outputBytes,
                                  NVX_3D_MAX_BUFFER_BYTES))
            return NVX_3D_STATUS_INVALID_PARAMETER;
        if(information)
            *information=((const NVX_3D_DOWNLOAD_BUFFER *)input)->Bytes;
        return NVX_3D_STATUS_SUCCESS;
    default:
        return NVX_3D_STATUS_INVALID_PARAMETER;
    }
}

#endif
