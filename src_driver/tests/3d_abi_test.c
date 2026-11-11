#include "../nvx_3d_abi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(NVX_3D_QUERY_CAPS)==56,
               "x86 and x64 capability ABI size must match");
_Static_assert(sizeof(NVX_3D_BATCH_HEADER)==40,
               "x86 and x64 batch header size must match");
_Static_assert(sizeof(NVX_3D_BUFFER_REFERENCE)==16,
               "x86 and x64 BO reference size must match");
_Static_assert(sizeof(NVX_3D_RELOCATION)==40,
               "x86 and x64 relocation size must match");

static uint32_t
build_test_batch(uint8_t *input, NVX_3D_ALLOC_BUFFER *allocation)
{
    NVX_3D_BATCH_HEADER header={0};
    uint32_t dwords[6];
    NVX_3D_BUFFER_REFERENCE reference={0};
    NVX_3D_RELOCATION relocs[2]={{0}};
    uint32_t inputBytes=(uint32_t)(sizeof(header)+sizeof(reference)+
                                   sizeof(dwords)+sizeof(relocs));

    header.Size=sizeof(header);
    header.Version=NVX_3D_ABI_VERSION;
    header.DwordCount=6;
    header.BufferReferenceCount=1;
    header.RelocationCount=2;
    header.ClientToken=0x123400000001ULL;
    reference.BufferId=allocation->BufferId;
    reference.Access=NVX_3D_RELOC_READ|NVX_3D_RELOC_WRITE;
    dwords[0]=Nvx3dNvc0SeqHeader(0x0100U,2U);
    dwords[1]=0;
    dwords[2]=0;
    dwords[3]=Nvx3dNvc0ImmediateHeader(0x0104U,1U);
    dwords[4]=Nvx3dNvc0NonIncHeader(0x0108U,1U);
    dwords[5]=0;

    relocs[0].CommandWord=1;
    relocs[0].Access=NVX_3D_RELOC_READ;
    relocs[0].BufferId=allocation->BufferId;
    relocs[0].BufferOffset=32;
    relocs[0].Bytes=4;
    relocs[1].CommandWord=5;
    relocs[1].Access=NVX_3D_RELOC_WRITE;
    relocs[1].BufferId=allocation->BufferId;
    relocs[1].BufferOffset=64;
    relocs[1].Bytes=8;

    memcpy(input,&header,sizeof(header));
    memcpy(input+sizeof(header),&reference,sizeof(reference));
    memcpy(input+sizeof(header)+sizeof(reference),dwords,sizeof(dwords));
    memcpy(input+sizeof(header)+sizeof(reference)+sizeof(dwords),
           relocs,sizeof(relocs));
    return inputBytes;
}

static void
test_nvc0_batch_validator(void)
{
    uint8_t input[sizeof(NVX_3D_BATCH_HEADER)+sizeof(NVX_3D_BUFFER_REFERENCE)+
                  6*sizeof(uint32_t)+
                  2*sizeof(NVX_3D_RELOCATION)];
    NVX_3D_ALLOC_BUFFER allocation={0};
    NVX_3D_BATCH_HEADER header;
    NVX_3D_BUFFER_REFERENCE reference;
    NVX_3D_RELOCATION reloc;
    uint32_t dword,inputBytes;
    uint32_t referenceOffset=(uint32_t)sizeof(header);
    uint32_t relocOffset=(uint32_t)(sizeof(header)+
        sizeof(NVX_3D_BUFFER_REFERENCE)+6*sizeof(uint32_t));
    const uint64_t token=0x123400000001ULL;

    allocation.Size=sizeof(allocation);
    allocation.Version=NVX_3D_ABI_VERSION;
    allocation.Bytes=4096;
    allocation.BufferId=17;
    allocation.GpuVa=0x23000000ULL;
    allocation.ClientToken=token;
    inputBytes=build_test_batch(input,&allocation);
    assert(Nvx3dValidateNvc0Batch(input,inputBytes,&allocation,1,token));

    assert(!Nvx3dValidateNvc0Batch(input,inputBytes-1,&allocation,1,token));
    assert(!Nvx3dValidateNvc0Batch(input,inputBytes,&allocation,1,token+1));

    inputBytes=build_test_batch(input,&allocation);
    memcpy(&header,input,sizeof(header));
    header.Reserved[0]=1;
    memcpy(input,&header,sizeof(header));
    assert(!Nvx3dValidateNvc0Batch(input,inputBytes,&allocation,1,token));

    inputBytes=build_test_batch(input,&allocation);
    memcpy(&header,input,sizeof(header));
    header.Reserved0=1;
    memcpy(input,&header,sizeof(header));
    assert(!Nvx3dValidateNvc0Batch(input,inputBytes,&allocation,1,token));

    inputBytes=build_test_batch(input,&allocation);
    memcpy(&reference,input+referenceOffset,sizeof(reference));
    reference.BufferId++;
    memcpy(input+referenceOffset,&reference,sizeof(reference));
    assert(!Nvx3dValidateNvc0Batch(input,inputBytes,&allocation,1,token));

    inputBytes=build_test_batch(input,&allocation);
    memcpy(&reference,input+referenceOffset,sizeof(reference));
    reference.Access=4U;
    memcpy(input+referenceOffset,&reference,sizeof(reference));
    assert(!Nvx3dValidateNvc0Batch(input,inputBytes,&allocation,1,token));

    inputBytes=build_test_batch(input,&allocation);
    memcpy(&reference,input+referenceOffset,sizeof(reference));
    reference.Access=NVX_3D_RELOC_READ;
    memcpy(input+referenceOffset,&reference,sizeof(reference));
    assert(!Nvx3dValidateNvc0Batch(input,inputBytes,&allocation,1,token));

    inputBytes=build_test_batch(input,&allocation);
    memcpy(&reference,input+referenceOffset,sizeof(reference));
    reference.Reserved=1;
    memcpy(input+referenceOffset,&reference,sizeof(reference));
    assert(!Nvx3dValidateNvc0Batch(input,inputBytes,&allocation,1,token));

    inputBytes=build_test_batch(input,&allocation);
    memcpy(&header,input,sizeof(header));
    header.BufferReferenceCount=NVX_3D_MAX_BATCH_BUFFERS+1U;
    memcpy(input,&header,sizeof(header));
    assert(!Nvx3dValidateNvc0Batch(input,inputBytes,&allocation,1,token));

    inputBytes=build_test_batch(input,&allocation);
    memcpy(&header,input,sizeof(header));
    header.RelocationCount=NVX_3D_MAX_BATCH_RELOCATIONS+1U;
    memcpy(input,&header,sizeof(header));
    assert(!Nvx3dValidateNvc0Batch(input,inputBytes,&allocation,1,token));

    inputBytes=build_test_batch(input,&allocation);
    dword=0;
    memcpy(input+sizeof(header),&dword,sizeof(dword));
    assert(!Nvx3dValidateNvc0Batch(input,inputBytes,&allocation,1,token));

    inputBytes=build_test_batch(input,&allocation);
    dword=Nvx3dNvc0SeqHeader(0x0100U,4U);
    memcpy(input+sizeof(header),&dword,sizeof(dword));
    assert(!Nvx3dValidateNvc0Batch(input,inputBytes,&allocation,1,token));

    inputBytes=build_test_batch(input,&allocation);
    dword=Nvx3dNvc0SeqHeader(0x7ffcU,2U);
    memcpy(input+sizeof(header),&dword,sizeof(dword));
    assert(!Nvx3dValidateNvc0Batch(input,inputBytes,&allocation,1,token));

    inputBytes=build_test_batch(input,&allocation);
    memcpy(&reloc,input+relocOffset,sizeof(reloc));
    reloc.CommandWord=0; /* packet header, not method payload */
    memcpy(input+relocOffset,&reloc,sizeof(reloc));
    assert(!Nvx3dValidateNvc0Batch(input,inputBytes,&allocation,1,token));

    inputBytes=build_test_batch(input,&allocation);
    memcpy(&reloc,input+relocOffset,sizeof(reloc));
    reloc.CommandWord=3; /* immediate packet has no payload */
    memcpy(input+relocOffset,&reloc,sizeof(reloc));
    assert(!Nvx3dValidateNvc0Batch(input,inputBytes,&allocation,1,token));

    inputBytes=build_test_batch(input,&allocation);
    memcpy(input+relocOffset+sizeof(reloc),input+relocOffset,sizeof(reloc));
    assert(!Nvx3dValidateNvc0Batch(input,inputBytes,&allocation,1,token));

    inputBytes=build_test_batch(input,&allocation);
    memcpy(&reloc,input+relocOffset,sizeof(reloc));
    reloc.BufferId++;
    memcpy(input+relocOffset,&reloc,sizeof(reloc));
    assert(!Nvx3dValidateNvc0Batch(input,inputBytes,&allocation,1,token));

    inputBytes=build_test_batch(input,&allocation);
    memcpy(&reloc,input+relocOffset,sizeof(reloc));
    reloc.BufferOffset=allocation.Bytes-2;
    reloc.Bytes=8;
    memcpy(input+relocOffset,&reloc,sizeof(reloc));
    assert(!Nvx3dValidateNvc0Batch(input,inputBytes,&allocation,1,token));

    inputBytes=build_test_batch(input,&allocation);
    memcpy(&reloc,input+relocOffset,sizeof(reloc));
    reloc.Access=4;
    memcpy(input+relocOffset,&reloc,sizeof(reloc));
    assert(!Nvx3dValidateNvc0Batch(input,inputBytes,&allocation,1,token));

    inputBytes=build_test_batch(input,&allocation);
    memcpy(&reloc,input+relocOffset,sizeof(reloc));
    reloc.Access=0;
    memcpy(input+relocOffset,&reloc,sizeof(reloc));
    assert(!Nvx3dValidateNvc0Batch(input,inputBytes,&allocation,1,token));

    inputBytes=build_test_batch(input,&allocation);
    memcpy(&reloc,input+relocOffset,sizeof(reloc));
    reloc.Reserved[1]=1;
    memcpy(input+relocOffset,&reloc,sizeof(reloc));
    assert(!Nvx3dValidateNvc0Batch(input,inputBytes,&allocation,1,token));

    inputBytes=build_test_batch(input,&allocation);
    allocation.ClientToken=token+1;
    assert(!Nvx3dValidateNvc0Batch(input,inputBytes,&allocation,1,token));
}

static void
test_nvc0_direct_address_validation(void)
{
    uint8_t input[sizeof(NVX_3D_BATCH_HEADER)+
                  sizeof(NVX_3D_BUFFER_REFERENCE)+4*sizeof(uint32_t)];
    NVX_3D_BATCH_HEADER header={0};
    NVX_3D_BUFFER_REFERENCE reference={0};
    NVX_3D_ALLOC_BUFFER allocation={0};
    uint32_t dwords[4]={0};
    uint32_t bytes;
    const uint64_t token=0x123400000001ULL;

    allocation.Size=sizeof(allocation);
    allocation.Version=NVX_3D_ABI_VERSION;
    allocation.Bytes=4096;
    allocation.BufferId=17;
    allocation.GpuVa=0x23000000ULL;
    allocation.ClientToken=token;
    reference.BufferId=allocation.BufferId;
    reference.Access=NVX_3D_RELOC_READ|NVX_3D_RELOC_WRITE;
    header.Size=sizeof(header);
    header.Version=NVX_3D_ABI_VERSION;
    header.DwordCount=3;
    header.BufferReferenceCount=1;
    header.ClientToken=token;
    dwords[0]=Nvx3dNvc0SeqHeader(0x0800U,2U);
    dwords[1]=(uint32_t)(allocation.GpuVa>>32);
    dwords[2]=(uint32_t)allocation.GpuVa;
    bytes=(uint32_t)(sizeof(header)+sizeof(reference)+3*sizeof(uint32_t));
    memcpy(input,&header,sizeof(header));
    memcpy(input+sizeof(header),&reference,sizeof(reference));
    memcpy(input+sizeof(header)+sizeof(reference),dwords,3*sizeof(uint32_t));
    assert(Nvx3dValidateNvc0Batch(input,bytes,&allocation,1,token));

    dwords[2]=0;
    memcpy(input+sizeof(header)+sizeof(reference),dwords,3*sizeof(uint32_t));
    assert(!Nvx3dValidateNvc0Batch(input,bytes,&allocation,1,token));
    dwords[2]=(uint32_t)allocation.GpuVa;

    dwords[2]=(uint32_t)(allocation.GpuVa+allocation.Bytes);
    memcpy(input+sizeof(header)+sizeof(reference),dwords,3*sizeof(uint32_t));
    assert(!Nvx3dValidateNvc0Batch(input,bytes,&allocation,1,token));

    dwords[2]=(uint32_t)allocation.GpuVa;
    reference.Access=NVX_3D_RELOC_READ;
    memcpy(input+sizeof(header),&reference,sizeof(reference));
    memcpy(input+sizeof(header)+sizeof(reference),dwords,3*sizeof(uint32_t));
    assert(!Nvx3dValidateNvc0Batch(input,bytes,&allocation,1,token));

    reference.Access=NVX_3D_RELOC_READ|NVX_3D_RELOC_WRITE;
    header.DwordCount=4;
    dwords[0]=Nvx3dNvc0SeqHeader(0x0800U,1U);
    dwords[1]=(uint32_t)(allocation.GpuVa>>32);
    dwords[2]=Nvx3dNvc0SeqHeader(0x0804U,1U);
    dwords[3]=(uint32_t)allocation.GpuVa;
    bytes=(uint32_t)(sizeof(header)+sizeof(reference)+4*sizeof(uint32_t));
    memcpy(input,&header,sizeof(header));
    memcpy(input+sizeof(header),&reference,sizeof(reference));
    memcpy(input+sizeof(header)+sizeof(reference),dwords,4*sizeof(uint32_t));
    assert(!Nvx3dValidateNvc0Batch(input,bytes,&allocation,1,token));

    header.DwordCount=3;
    dwords[0]=Nvx3dNvc0NonIncHeader(0x0800U,2U);
    dwords[1]=(uint32_t)(allocation.GpuVa>>32);
    dwords[2]=(uint32_t)allocation.GpuVa;
    bytes=(uint32_t)(sizeof(header)+sizeof(reference)+3*sizeof(uint32_t));
    memcpy(input,&header,sizeof(header));
    memcpy(input+sizeof(header)+sizeof(reference),dwords,3*sizeof(uint32_t));
    assert(!Nvx3dValidateNvc0Batch(input,bytes,&allocation,1,token));
}

static void
build_nvc0_clear(NVX_3D_NVC0_CLEAR *request)
{
    const uint64_t gpuVa=0x23000000ULL;
    memset(request,0,sizeof(*request));
    request->Size=sizeof(*request);
    request->Version=NVX_3D_ABI_VERSION;
    request->DwordCount=NVX_3D_NVC0_CLEAR_DWORDS;
    request->BufferId=17;
    request->Dwords[0]=Nvx3dNvc0SeqHeader(0x0d80U,4U);
    request->Dwords[1]=0x3f000000U; /* R = 0.5 */
    request->Dwords[2]=0x3e800000U; /* G = 0.25 */
    request->Dwords[3]=0x3f800000U; /* B = 1.0 */
    request->Dwords[4]=0x3f800000U; /* A = 1.0 */
    request->Dwords[5]=Nvx3dNvc0SeqHeader(0x0ff4U,2U);
    request->Dwords[6]=64U<<16; /* full-width scissor at x=0 */
    request->Dwords[7]=16U<<16; /* full-height scissor at y=0 */
    request->Dwords[8]=Nvx3dNvc0SeqHeader(0x0d6cU,2U);
    request->Dwords[9]=64U<<16; /* full-width clear rect */
    request->Dwords[10]=16U<<16; /* full-height clear rect */
    request->Dwords[11]=Nvx3dNvc0SeqHeader(0x10f8U,1U);
    request->Dwords[12]=0x10U; /* use the validated full-target clear rect */
    request->Dwords[13]=Nvx3dNvc0SeqHeader(0x121cU,1U);
    request->Dwords[14]=1U; /* one render target */
    request->Dwords[15]=Nvx3dNvc0SeqHeader(0x0800U,9U);
    request->Dwords[16]=(uint32_t)(gpuVa>>32);
    request->Dwords[17]=(uint32_t)gpuVa;
    request->Dwords[18]=64U;
    request->Dwords[19]=16U;
    request->Dwords[20]=0xcfU; /* BGRA8 UNORM */
    request->Dwords[21]=0x10U; /* one-GOB block height */
    request->Dwords[22]=1U; /* one layer */
    request->Dwords[23]=65536U/4U;
    request->Dwords[24]=0U; /* base layer */
    request->Dwords[25]=Nvx3dNvc0ImmediateHeader(0x15d0U,0U);
    request->Dwords[26]=Nvx3dNvc0NonIncHeader(0x19d0U,1U);
    request->Dwords[27]=0x3cU; /* RGBA clear only */
}

int main(void)
{
    NVX_3D_ALLOC_BUFFER alloc={0};
    NVX_3D_FREE_BUFFER freeRequest={0};
    NVX_3D_UPLOAD_BUFFER upload={0};
    NVX_3D_DOWNLOAD_BUFFER download={0};
    NVX_3D_SUBMIT submit={0};
    NVX_3D_WAIT_FENCE wait={0};
    NVX_3D_READBACK readback={0};
    uint32_t readbackPixels[NVX_3D_READBACK_PIXELS];
    NVX_3D_QUERY_CAPS caps;
    NVX_3D_COLOR_SURFACE_LAYOUT surface;
    NVX_3D_DEPTH_SURFACE_LAYOUT depthSurface;
    NVX_3D_NVC0_CLEAR nvc0Clear, nvc0Bad;
    uint32_t nvc0Color=0;
    uint32_t information=99;

    assert(sizeof(NVX_3D_QUERY_CAPS)==56);
    assert(Nvx3dPackPciLocation(0x2aU,0xabU)==
           (NVX_3D_PCI_LOCATION_VALID|0xab2aU));
    assert(sizeof(NVX_3D_ALLOC_BUFFER)==56);
    assert(sizeof(NVX_3D_BATCH_HEADER)==40);
    assert(sizeof(NVX_3D_RELOCATION)==40);
    assert(sizeof(NVX_3D_FREE_BUFFER)==16);
    assert(sizeof(NVX_3D_UPLOAD_BUFFER)==32);
    assert(sizeof(NVX_3D_DOWNLOAD_BUFFER)==32);
    assert(sizeof(NVX_3D_SUBMIT)==72);
    assert(sizeof(NVX_3D_WAIT_FENCE)==24);
    assert(sizeof(NVX_3D_READBACK)==32);
    assert(sizeof(NVX_3D_COLOR_SURFACE_LAYOUT)==24);
    assert(sizeof(NVX_3D_NVC0_CLEAR)==152);
    test_nvc0_batch_validator();
    test_nvc0_direct_address_validation();

    download.Size=sizeof(download);
    download.Version=NVX_3D_ABI_VERSION;
    download.BufferId=17;
    download.Offset=256;
    download.Bytes=4096;
    assert(Nvx3dValidateDownload(&download,sizeof(download),4096,8192));
    assert(Nvx3dValidateDownload(&download,sizeof(download),8192,8192));
    assert(!Nvx3dValidateDownload(&download,sizeof(download),4095,8192));
    assert(!Nvx3dValidateDownload(&download,sizeof(download),4096,4096));
    assert(!Nvx3dValidateDownload(&download,sizeof(download)-4,4096,8192));
    assert(!Nvx3dValidateDownload(&download,sizeof(download),4096,0));
    download.Offset=2;
    assert(!Nvx3dValidateDownload(&download,sizeof(download),4096,8192));
    download.Offset=256;download.Bytes=4097;
    assert(!Nvx3dValidateDownload(&download,sizeof(download),8192,8192));
    download.Bytes=4094;
    assert(!Nvx3dValidateDownload(&download,sizeof(download),4094,8192));
    download.Bytes=4096;download.Reserved[0]=1;
    assert(!Nvx3dValidateDownload(&download,sizeof(download),4096,8192));
    download.Reserved[0]=0;
    assert(Nvx3dDispatch(NVX_3D_OP_DOWNLOAD,&download,sizeof(download),
        readbackPixels,sizeof(readbackPixels),&information,
        0,0,0,0,0,0)==NVX_3D_STATUS_SUCCESS);
    assert(information==download.Bytes);
    assert(Nvx3dDispatch(NVX_3D_OP_DOWNLOAD,&download,sizeof(download),
        readbackPixels,download.Bytes-1,&information,
        0,0,0,0,0,0)==NVX_3D_STATUS_INVALID_PARAMETER);
    assert(Nvx3dNvc0Unorm8(0x00000000U)==0);
    assert(Nvx3dNvc0Unorm8(0x3e800000U)==64);
    assert(Nvx3dNvc0Unorm8(0x3f000000U)==128);
    assert(Nvx3dNvc0Unorm8(0x3f800000U)==255);

    assert(Nvx3dComputeColorSurfaceLayout(64,16,&surface));
    assert(surface.PitchBytes==256&&surface.TileMode==0x10&&
           surface.Bytes==65536&&surface.Reserved==0);
    assert(Nvx3dComputeColorSurfaceLayout(632,453,&surface));
    assert(surface.Width==632&&surface.Height==453&&
           surface.PitchBytes==2560&&surface.TileMode==0x40&&
           surface.Bytes==1310720);
    assert(Nvx3dChipsetFromDeviceId(0x2484)==0x170);
    assert(Nvx3dChipsetFromDeviceId(0x2488)==0x170);
    assert(Nvx3dChipsetFromDeviceId(0x249d)==0x170);
    assert(Nvx3dChipsetFromDeviceId(0x2487)==0);
    assert(Nvx3dTuringColorPixelOffset(0,0,632,0x40)==0);
    assert(Nvx3dTuringColorPixelOffset(1,0,632,0x40)==4);
    assert(Nvx3dTuringColorPixelOffset(0,1,632,0x40)==16);
    assert(Nvx3dTuringColorPixelOffset(0,4,632,0x40)==128);
    assert(Nvx3dTuringColorPixelOffset(4,0,632,0x40)==64);
    assert(Nvx3dTuringColorPixelOffset(8,0,632,0x40)==256);
    assert(Nvx3dTuringColorPixelOffset(0,8,632,0x40)==512);
    assert(Nvx3dTuringColorPixelOffset(0,128,632,0x40)==327680);
    assert(Nvx3dComputeColorSurfaceLayout(4096,4096,&surface));
    assert(surface.PitchBytes==16384&&surface.TileMode==0x40&&
           surface.Bytes==NVX_3D_MAX_BUFFER_BYTES);
    assert(!Nvx3dComputeColorSurfaceLayout(0,453,&surface));
    assert(!Nvx3dComputeColorSurfaceLayout(632,0,&surface));
    assert(!Nvx3dComputeColorSurfaceLayout(4097,453,&surface));
    assert(!Nvx3dComputeColorSurfaceLayout(4096,4097,&surface));
    assert(!Nvx3dComputeColorSurfaceLayout(4096,4096,NULL));
    assert(Nvx3dComputeDepthSurfaceLayout(64,16,&depthSurface));
    assert(depthSurface.PitchBytes==256&&depthSurface.Bytes==65536);
    assert(Nvx3dComputeDepthSurfaceLayout(632,453,&depthSurface));
    assert(depthSurface.PitchBytes==2560&&depthSurface.Bytes==1179648);
    assert(!Nvx3dComputeDepthSurfaceLayout(4097,453,&depthSurface));
    assert(!Nvx3dComputeDepthSurfaceLayout(632,0,&depthSurface));

    build_nvc0_clear(&nvc0Clear);
    assert(Nvx3dValidateNvc0Clear(&nvc0Clear,sizeof(nvc0Clear),
        0x23000000ULL,65536,64,16,256,0x10,&nvc0Color));
    assert(nvc0Color==0xff8040ffU);
    assert(!Nvx3dValidateNvc0Clear(&nvc0Clear,sizeof(nvc0Clear)-4,
        0x23000000ULL,65536,64,16,256,0x10,&nvc0Color));
    assert(!Nvx3dValidateNvc0Clear(&nvc0Clear,sizeof(nvc0Clear),
        0x23010000ULL,65536,64,16,256,0x10,&nvc0Color));
    assert(!Nvx3dValidateNvc0Clear(&nvc0Clear,sizeof(nvc0Clear),
        0x23000000ULL,65535,64,16,256,0x10,&nvc0Color));
    assert(!Nvx3dValidateNvc0Clear(&nvc0Clear,sizeof(nvc0Clear),
        0x23000000ULL,65536,64,16,260,0x10,&nvc0Color));
    assert(!Nvx3dValidateNvc0Clear(&nvc0Clear,sizeof(nvc0Clear),
        0x23000000ULL,65536,64,16,256,0x20,&nvc0Color));
#define REJECT_NVC0_CLEAR(_word, _value) do { \
        nvc0Bad=nvc0Clear; nvc0Bad.Dwords[(_word)]=(_value); \
        assert(!Nvx3dValidateNvc0Clear(&nvc0Bad,sizeof(nvc0Bad), \
            0x23000000ULL,65536,64,16,256,0x10,&nvc0Color)); \
    } while(0)
    REJECT_NVC0_CLEAR(0,0x40000000U); /* not an NVC0 sequential packet */
    REJECT_NVC0_CLEAR(5,Nvx3dNvc0SeqHeader(0x0ff8U,2U)); /* other method */
    REJECT_NVC0_CLEAR(1,0xbf000000U); /* negative clear component */
    REJECT_NVC0_CLEAR(2,0x7fc00000U); /* NaN component */
    REJECT_NVC0_CLEAR(3,0x3f800001U); /* component above one */
    REJECT_NVC0_CLEAR(6,(64U<<16)|1U); /* partial scissor */
    REJECT_NVC0_CLEAR(7,15U<<16); /* partial scissor */
    REJECT_NVC0_CLEAR(9,(63U<<16)); /* partial clear rectangle */
    REJECT_NVC0_CLEAR(10,(15U<<16)); /* partial clear rectangle */
    REJECT_NVC0_CLEAR(12,0x11U); /* unsupported clear-rectangle control */
    REJECT_NVC0_CLEAR(14,0U); /* no target */
    REJECT_NVC0_CLEAR(17,0x23000004U); /* unvalidated address */
    REJECT_NVC0_CLEAR(18,63U); /* target extent mismatch */
    REJECT_NVC0_CLEAR(20,0U); /* unsupported format */
    REJECT_NVC0_CLEAR(21,0x20U); /* mismatched tiling */
    REJECT_NVC0_CLEAR(22,2U); /* layered target */
    REJECT_NVC0_CLEAR(23,65536U/8U); /* invalid layer stride */
    REJECT_NVC0_CLEAR(24,1U); /* nonzero base layer */
    REJECT_NVC0_CLEAR(25,Nvx3dNvc0ImmediateHeader(0x15d0U,1U));
    REJECT_NVC0_CLEAR(27,0x3dU); /* depth/stencil or extra RT clear */
#undef REJECT_NVC0_CLEAR
    nvc0Bad=nvc0Clear;nvc0Bad.FenceValue=1;
    assert(!Nvx3dValidateNvc0Clear(&nvc0Bad,sizeof(nvc0Bad),
        0x23000000ULL,65536,64,16,256,0x10,NULL));
    nvc0Bad=nvc0Clear;nvc0Bad.Reserved=1;
    assert(!Nvx3dValidateNvc0Clear(&nvc0Bad,sizeof(nvc0Bad),
        0x23000000ULL,65536,64,16,256,0x10,NULL));
    nvc0Bad=nvc0Clear;nvc0Bad.Reserved2=1;
    assert(!Nvx3dValidateNvc0Clear(&nvc0Bad,sizeof(nvc0Bad),
        0x23000000ULL,65536,64,16,256,0x10,NULL));
    nvc0Bad=nvc0Clear;nvc0Bad.DwordCount++;
    assert(!Nvx3dValidateNvc0Clear(&nvc0Bad,sizeof(nvc0Bad),
        0x23000000ULL,65536,64,16,256,0x10,NULL));

    Nvx3dBuildCaps(&caps,0,0,0,0,0,0,0,0);
    assert(caps.Size==sizeof(caps)&&caps.Version==NVX_3D_ABI_VERSION);
    assert(caps.GraphicsClass==0xC797&&caps.PageBytes==NVX_3D_PAGE_BYTES);
    assert(caps.Features==0&&caps.MaxPushDwords==NVX_3D_MAX_PUSH_DWORDS&&
        caps.MaxBufferBytes==NVX_3D_MAX_BUFFER_BYTES);
    Nvx3dBuildCaps(&caps,1,1,0,0,1,1,1,1);
    assert(caps.Features==(NVX_3D_FEATURE_GR_CHANNEL|
        NVX_3D_FEATURE_I2M_VERIFIED|NVX_3D_FEATURE_CLEAR_VERIFIED|
        NVX_3D_FEATURE_USER_ALLOC|NVX_3D_FEATURE_UPLOAD|
        NVX_3D_FEATURE_DOWNLOAD|NVX_3D_FEATURE_FIXED_PROFILE_SUBMIT));
    assert(!(caps.Features&NVX_3D_FEATURE_NVC0_BATCH_SUBMIT));
    assert(Nvx3dDispatch(NVX_3D_OP_QUERY_CAPS,NULL,0,&caps,sizeof(caps),
        &information,1,1,0,0,1,1)==NVX_3D_STATUS_SUCCESS);
    assert(information==sizeof(caps)&&caps.Features!=0);
    assert(Nvx3dDispatch(NVX_3D_OP_QUERY_CAPS,NULL,0,&caps,sizeof(caps)-1,
        &information,1,1,0,0,1,1)==NVX_3D_STATUS_BUFFER_TOO_SMALL);
    assert(information==0);
    assert(Nvx3dDispatch(NVX_3D_OP_QUERY_CAPS,NULL,0,&caps,sizeof(caps),
        &information,1,1,0,1,1,1)==NVX_3D_STATUS_SUCCESS);
    assert(caps.Features==0);

    alloc.Size=sizeof(alloc);alloc.Version=NVX_3D_ABI_VERSION;
    alloc.Usage=3;alloc.Bytes=NVX_3D_DEPTH_TARGET_TOTAL_BYTES;
    assert(Nvx3dValidateAlloc(&alloc));
    assert(Nvx3dComputeColorSurfaceLayout(632,453,&surface));
    alloc.Usage=NVX_3D_USAGE_COLOR_TARGET;alloc.Bytes=surface.Bytes;
    alloc.Width=surface.Width;alloc.Height=surface.Height;
    alloc.PitchBytes=surface.PitchBytes;alloc.TileMode=surface.TileMode;
    assert(Nvx3dValidateAlloc(&alloc));
    alloc.PitchBytes++;
    assert(!Nvx3dValidateAlloc(&alloc));
    alloc.PitchBytes=surface.PitchBytes;alloc.TileMode++;
    assert(!Nvx3dValidateAlloc(&alloc));
    alloc.TileMode=surface.TileMode;alloc.Bytes-=NVX_3D_SURFACE_ALIGNMENT;
    assert(!Nvx3dValidateAlloc(&alloc));
    alloc.Bytes=surface.Bytes;alloc.Usage|=NVX_3D_USAGE_DEPTH_TARGET;
    assert(!Nvx3dValidateAlloc(&alloc));
    alloc.Usage=3;alloc.Bytes=65536;alloc.Width=alloc.Height=0;
    alloc.PitchBytes=alloc.TileMode=0;
    assert(!Nvx3dValidateAlloc(&alloc));
    alloc.Bytes=NVX_3D_DEPTH_TARGET_TOTAL_BYTES;
    assert(Nvx3dValidateAlloc(&alloc));
    alloc.Usage=7;alloc.Bytes=NVX_3D_DEPTH_TARGET_TOTAL_BYTES;
    assert(Nvx3dValidateAlloc(&alloc));
    alloc.Usage=15;
    assert(Nvx3dValidateAlloc(&alloc));
    alloc.Usage=NVX_3D_USAGE_DEPTH_TARGET|NVX_3D_USAGE_DEPTH_ZF32;
    alloc.Bytes=NVX_3D_DEPTH_TARGET_TOTAL_BYTES;
    assert(Nvx3dValidateAlloc(&alloc));
    assert(Nvx3dComputeDepthSurfaceLayout(632,453,&depthSurface));
    alloc.Width=depthSurface.Width;alloc.Height=depthSurface.Height;
    alloc.PitchBytes=depthSurface.PitchBytes;
    alloc.Bytes=NVX_3D_DEPTH_TARGET_OFFSET+depthSurface.Bytes;
    assert(Nvx3dValidateAlloc(&alloc));
    alloc.PitchBytes++;
    assert(!Nvx3dValidateAlloc(&alloc));
    alloc.PitchBytes=depthSurface.PitchBytes;
    alloc.TileMode=1;
    assert(!Nvx3dValidateAlloc(&alloc));
    alloc.Width=alloc.Height=alloc.PitchBytes=alloc.TileMode=0;
    alloc.Bytes=NVX_3D_DEPTH_TARGET_TOTAL_BYTES;
    alloc.Usage=NVX_3D_USAGE_DEPTH_TARGET|NVX_3D_USAGE_DEPTH_ZF32|
                NVX_3D_USAGE_DEPTH_S8Z24;
    assert(!Nvx3dValidateAlloc(&alloc));
    alloc.Usage=4;assert(!Nvx3dValidateAlloc(&alloc));
    alloc.Usage=8;assert(!Nvx3dValidateAlloc(&alloc));
    alloc.Usage=3;alloc.Bytes=NVX_3D_DEPTH_TARGET_TOTAL_BYTES;
    assert(Nvx3dDispatch(NVX_3D_OP_ALLOC,&alloc,sizeof(alloc),NULL,0,NULL,
        0,0,0,0,0,0)==NVX_3D_STATUS_SUCCESS);
    alloc.Bytes=65535;assert(!Nvx3dValidateAlloc(&alloc));
    alloc.Bytes=NVX_3D_MAX_BUFFER_BYTES+4096;assert(!Nvx3dValidateAlloc(&alloc));
    alloc.Bytes=4096;alloc.BufferId=1;assert(!Nvx3dValidateAlloc(&alloc));

    upload.Size=sizeof(upload);upload.Version=NVX_3D_ABI_VERSION;
    upload.BufferId=1;upload.Offset=128;upload.Bytes=256;
    assert(Nvx3dValidateUpload(&upload,sizeof(upload)+upload.Bytes,4096));
    assert(!Nvx3dValidateUpload(&upload,sizeof(upload)+upload.Bytes-1,4096));
    assert(!Nvx3dValidateUpload(&upload,sizeof(upload)+upload.Bytes,383));
    upload.Offset++;
    assert(!Nvx3dValidateUpload(&upload,sizeof(upload)+upload.Bytes,4096));
    upload.Offset--;
    upload.Bytes--;
    assert(!Nvx3dValidateUpload(&upload,sizeof(upload)+upload.Bytes,4096));
    upload.Bytes++;
    upload.Offset=UINT32_MAX-64;
    assert(!Nvx3dValidateUpload(&upload,sizeof(upload)+upload.Bytes,4096));
    upload.Offset=0;upload.Bytes=NVX_3D_MAX_UPLOAD_BYTES+1;
    assert(!Nvx3dValidateUpload(&upload,sizeof(upload)+upload.Bytes,65536));
    upload.Bytes=256;upload.Reserved[0]=1;
    assert(!Nvx3dValidateUpload(&upload,sizeof(upload)+upload.Bytes,4096));
    upload.Reserved[0]=0;

    submit.Size=sizeof(submit);submit.Version=NVX_3D_ABI_VERSION;
    submit.Operation=NVX_3D_SUBMIT_CLEAR_SURFACE;
    submit.Width=64;submit.Height=16;submit.BufferId=1;
    assert(Nvx3dValidateSubmit(&submit,sizeof(submit),65536));
    submit.Color=0xffffffffU;
    assert(Nvx3dValidateSubmit(&submit,sizeof(submit),65536));
    submit.Color=0x12345678U;
    assert(Nvx3dValidateSubmit(&submit,sizeof(submit),65536));
    submit.Color=0;
    assert(Nvx3dDispatch(NVX_3D_OP_SUBMIT,&submit,sizeof(submit),NULL,0,NULL,
        0,0,0,0,0,0)==NVX_3D_STATUS_SUCCESS);
    submit.Operation=NVX_3D_SUBMIT_TEST_TRIANGLE;
    submit.Color=0x12345678U;
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),65536));
    submit.Color=0;
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),65535));
    assert(Nvx3dValidateSubmit(&submit,sizeof(submit),65536));
    assert(Nvx3dDispatch(NVX_3D_OP_SUBMIT,&submit,sizeof(submit),NULL,0,NULL,
        0,0,0,0,0,0)==NVX_3D_STATUS_SUCCESS);
    submit.Operation=NVX_3D_SUBMIT_DRAW_TRIANGLE;
    submit.Reserved[0]=2;submit.Color=0xff00ff00U;
    assert(Nvx3dValidateSubmit(&submit,sizeof(submit),65536));
    assert(Nvx3dSubmitVertexBufferId(&submit)==2);
    assert(Nvx3dDispatch(NVX_3D_OP_SUBMIT,&submit,sizeof(submit),NULL,0,NULL,
        0,0,0,0,0,0)==NVX_3D_STATUS_SUCCESS);
    submit.Operation=NVX_3D_SUBMIT_DRAW_TRIANGLE_PRESERVE;
    submit.Color=0xffff0000U;
    assert(Nvx3dValidateSubmit(&submit,sizeof(submit),65536));
    assert(Nvx3dDispatch(NVX_3D_OP_SUBMIT,&submit,sizeof(submit),NULL,0,NULL,
        0,0,0,0,0,0)==NVX_3D_STATUS_SUCCESS);
    submit.Operation=NVX_3D_SUBMIT_DRAW_TRIANGLES;
    submit.Height=36;
    assert(Nvx3dValidateSubmit(&submit,sizeof(submit),65536));
    submit.Height=4;
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),65536));
    submit.Height=39;
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),65536));
    submit.Height=16;
    submit.Operation=NVX_3D_SUBMIT_CLEAR_DEPTH;
    submit.Color=0;submit.Reserved[0]=0;
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),
                                NVX_3D_DEPTH_TARGET_TOTAL_BYTES-1));
    assert(Nvx3dValidateSubmit(&submit,sizeof(submit),
                               NVX_3D_DEPTH_TARGET_TOTAL_BYTES));
    assert(Nvx3dRequiredSubmitBytes(submit.Operation)==
           NVX_3D_DEPTH_TARGET_TOTAL_BYTES);
    submit.Operation=NVX_3D_SUBMIT_DRAW_TRIANGLE_DEPTH;
    submit.Reserved[0]=2;
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),
                                NVX_3D_DEPTH_TARGET_TOTAL_BYTES-1));
    assert(Nvx3dValidateSubmit(&submit,sizeof(submit),
                               NVX_3D_DEPTH_TARGET_TOTAL_BYTES));
    assert(Nvx3dDispatch(NVX_3D_OP_SUBMIT,&submit,sizeof(submit),NULL,0,NULL,
        0,0,0,0,0,0)==NVX_3D_STATUS_SUCCESS);
    assert(Nvx3dSubmitVertexBufferId(&submit)==2);
    assert(Nvx3dRequiredSubmitBytes(submit.Operation)==
           NVX_3D_DEPTH_TARGET_TOTAL_BYTES);
    submit.Reserved[0]=1;
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),
                                NVX_3D_DEPTH_TARGET_TOTAL_BYTES));
    submit.Reserved[0]=0;submit.Operation=NVX_3D_SUBMIT_CLEAR_DEPTH;
    submit.Color=0x01020304U;
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),
                                NVX_3D_DEPTH_TARGET_TOTAL_BYTES));
    submit.Color=0;submit.Reserved[0]=1;
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),
                                NVX_3D_DEPTH_TARGET_TOTAL_BYTES));
    submit.Reserved[0]=0;
    submit.Operation=NVX_3D_SUBMIT_DRAW_TRIANGLE_PRESERVE;
    submit.Color=0xffff0000U;
    submit.Reserved[0]=1;
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),65536));
    submit.Reserved[0]=0;
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),65536));
    submit.Operation=NVX_3D_SUBMIT_TEST_TRIANGLE;submit.Color=0;
    submit.Operation=99;
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),65536));
    submit.Operation=NVX_3D_SUBMIT_CLEAR_SURFACE;
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),4095));
    submit.Width=65;assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),65536));
    submit.Width=64;submit.Reserved[0]=1;
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),65536));
    submit.Reserved[0]=0;
    submit.Operation=NVX_3D_SUBMIT_CLEAR_COLOR_TARGET;
    submit.Width=632;submit.Height=453;submit.Color=0xff204080U;
    assert(Nvx3dComputeColorSurfaceLayout(632,453,&surface));
    assert(Nvx3dValidateSubmit(&submit,sizeof(submit),surface.Bytes));
    submit.Reserved[0]=1;
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),surface.Bytes));
    submit.Reserved[0]=0;submit.Width=4097;
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),surface.Bytes));
    submit.Operation=NVX_3D_SUBMIT_DRAW_COLOR_TARGET;
    submit.Width=surface.Width;submit.Height=surface.Height;
    submit.Color=0xff20d040U;submit.BufferId=1;
    submit.Reserved[0]=6;submit.Reserved[1]=2;
    submit.Reserved[2]=0;submit.Reserved[3]=surface.TileMode;
    assert(Nvx3dValidateSubmit(&submit,sizeof(submit),surface.Bytes));
    assert(Nvx3dSubmitVertexBufferId(&submit)==2);
    submit.Operation=NVX_3D_SUBMIT_VERIFY_COLOR_TARGET_DRAW;
    assert(Nvx3dValidateSubmit(&submit,sizeof(submit),surface.Bytes));
    submit.Operation=NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH;
    submit.Reserved[4]=3;submit.Reserved[5]=0;
    assert(Nvx3dValidateSubmit(&submit,sizeof(submit),surface.Bytes));
    assert(Nvx3dDepthModeForOperation(submit.Operation)==5U);
    submit.Operation=NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH_ATTACH;
    assert(Nvx3dValidateSubmit(&submit,sizeof(submit),surface.Bytes));
    assert(Nvx3dDepthModeForOperation(submit.Operation)==2U);
    submit.Operation=NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH_TEST_ONLY;
    assert(Nvx3dValidateSubmit(&submit,sizeof(submit),surface.Bytes));
    assert(Nvx3dDepthModeForOperation(submit.Operation)==3U);
    submit.Operation=NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH;
    submit.Reserved[4]=2;
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),surface.Bytes));
    submit.Reserved[4]=3;submit.Reserved[5]=0;
    submit.Operation=NVX_3D_SUBMIT_CLEAR_COLOR_TARGET_DEPTH;
    memset(submit.Reserved,0,sizeof(submit.Reserved));
    assert(Nvx3dValidateSubmit(&submit,sizeof(submit),surface.Bytes));
    submit.Reserved[0]=1;
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),surface.Bytes));
    submit.Reserved[0]=0;
    submit.Operation=NVX_3D_SUBMIT_DRAW_COLOR_TARGET;
    submit.Reserved[0]=5;
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),surface.Bytes));
    submit.Reserved[0]=6;submit.Reserved[3]=0;
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),surface.Bytes));
    submit.Reserved[3]=surface.TileMode;submit.Reserved[1]=0;
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),surface.Bytes));

    wait.Size=sizeof(wait);wait.Version=NVX_3D_ABI_VERSION;
    wait.FenceValue=1;wait.TimeoutMs=30000;
    assert(Nvx3dValidateWait(&wait));
    assert(Nvx3dDispatch(NVX_3D_OP_WAIT,&wait,sizeof(wait),NULL,0,NULL,
        0,0,0,0,0,0)==NVX_3D_STATUS_SUCCESS);
    wait.TimeoutMs=30001;assert(!Nvx3dValidateWait(&wait));

    freeRequest.Size=sizeof(freeRequest);freeRequest.Version=NVX_3D_ABI_VERSION;
    freeRequest.BufferId=1;assert(Nvx3dValidateFree(&freeRequest));
    assert(Nvx3dDispatch(NVX_3D_OP_FREE,&freeRequest,sizeof(freeRequest),NULL,0,
        NULL,0,0,0,0,0,0)==NVX_3D_STATUS_SUCCESS);
    freeRequest.BufferId=0;assert(!Nvx3dValidateFree(&freeRequest));

    readback.Size=sizeof(readback);readback.Version=NVX_3D_ABI_VERSION;
    readback.BufferId=1;readback.Width=NVX_3D_TRIANGLE_WIDTH;
    readback.Height=NVX_3D_TRIANGLE_HEIGHT;
    assert(Nvx3dValidateReadback(&readback,NVX_3D_READBACK_BYTES,
                                 NVX_3D_TRIANGLE_TARGET_BYTES));
    assert(Nvx3dDispatch(NVX_3D_OP_READBACK,&readback,sizeof(readback),
        readbackPixels,sizeof(readbackPixels),&information,0,0,0,0,0,0)==
        NVX_3D_STATUS_SUCCESS);
    assert(information==NVX_3D_READBACK_BYTES);
    assert(!Nvx3dValidateReadback(&readback,NVX_3D_READBACK_BYTES-1,
                                  NVX_3D_TRIANGLE_TARGET_BYTES));
    readback.Width++;
    assert(!Nvx3dValidateReadback(&readback,NVX_3D_READBACK_BYTES,
                                  NVX_3D_TRIANGLE_TARGET_BYTES));
    readback.Width--;
    readback.Reserved[1]=1;
    assert(!Nvx3dValidateReadback(&readback,NVX_3D_READBACK_BYTES,
                                  NVX_3D_TRIANGLE_TARGET_BYTES));
    readback.Reserved[1]=16;
    assert(Nvx3dValidateReadback(&readback,NVX_3D_READBACK_BYTES,
                                  NVX_3D_TRIANGLE_TARGET_BYTES));
    readback.Reserved[0]=1;
    assert(!Nvx3dValidateReadback(&readback,NVX_3D_READBACK_BYTES,
                                  NVX_3D_TRIANGLE_TARGET_BYTES));

    puts("3D ABI layout, capabilities, ownership and command checks passed");
    return 0;
}
