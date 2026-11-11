#include "../nvx_3d_abi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

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
    request->Dwords[8]=Nvx3dNvc0SeqHeader(0x121cU,1U);
    request->Dwords[9]=1U; /* one render target */
    request->Dwords[10]=Nvx3dNvc0SeqHeader(0x0800U,9U);
    request->Dwords[11]=(uint32_t)(gpuVa>>32);
    request->Dwords[12]=(uint32_t)gpuVa;
    request->Dwords[13]=64U;
    request->Dwords[14]=16U;
    request->Dwords[15]=0xcfU; /* BGRA8 UNORM */
    request->Dwords[16]=0x10U; /* one-GOB block height */
    request->Dwords[17]=1U; /* one layer */
    request->Dwords[18]=65536U/4U;
    request->Dwords[19]=0U; /* base layer */
    request->Dwords[20]=Nvx3dNvc0ImmediateHeader(0x15d0U,0U);
    request->Dwords[21]=Nvx3dNvc0NonIncHeader(0x19d0U,1U);
    request->Dwords[22]=0x3cU; /* RGBA clear only */
}

int main(void)
{
    NVX_3D_ALLOC_BUFFER alloc={0};
    NVX_3D_FREE_BUFFER freeRequest={0};
    NVX_3D_UPLOAD_BUFFER upload={0};
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

    assert(sizeof(NVX_3D_QUERY_CAPS)==32);
    assert(sizeof(NVX_3D_ALLOC_BUFFER)==56);
    assert(sizeof(NVX_3D_FREE_BUFFER)==16);
    assert(sizeof(NVX_3D_UPLOAD_BUFFER)==32);
    assert(sizeof(NVX_3D_SUBMIT)==72);
    assert(sizeof(NVX_3D_WAIT_FENCE)==24);
    assert(sizeof(NVX_3D_READBACK)==32);
    assert(sizeof(NVX_3D_COLOR_SURFACE_LAYOUT)==24);
    assert(sizeof(NVX_3D_NVC0_CLEAR)==128);
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
    REJECT_NVC0_CLEAR(9,0U); /* no target */
    REJECT_NVC0_CLEAR(12,0x23000004U); /* unvalidated address */
    REJECT_NVC0_CLEAR(13,63U); /* target extent mismatch */
    REJECT_NVC0_CLEAR(15,0U); /* unsupported format */
    REJECT_NVC0_CLEAR(16,0x20U); /* mismatched tiling */
    REJECT_NVC0_CLEAR(17,2U); /* layered target */
    REJECT_NVC0_CLEAR(18,65536U/8U); /* invalid layer stride */
    REJECT_NVC0_CLEAR(19,1U); /* nonzero base layer */
    REJECT_NVC0_CLEAR(20,Nvx3dNvc0ImmediateHeader(0x15d0U,1U));
    REJECT_NVC0_CLEAR(22,0x3dU); /* depth/stencil or extra RT clear */
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
        NVX_3D_FEATURE_SUBMIT));
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
