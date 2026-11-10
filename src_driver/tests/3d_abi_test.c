#include "../nvx_3d_abi.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    NVX_3D_ALLOC_BUFFER alloc={0};
    NVX_3D_FREE_BUFFER freeRequest={0};
    NVX_3D_SUBMIT submit={0};
    NVX_3D_WAIT_FENCE wait={0};
    NVX_3D_QUERY_CAPS caps;
    uint32_t information=99;

    assert(sizeof(NVX_3D_QUERY_CAPS)==32);
    assert(sizeof(NVX_3D_ALLOC_BUFFER)==32);
    assert(sizeof(NVX_3D_FREE_BUFFER)==16);
    assert(sizeof(NVX_3D_SUBMIT)==48);
    assert(sizeof(NVX_3D_WAIT_FENCE)==24);

    Nvx3dBuildCaps(&caps,0,0,0,0,0,0,0,0);
    assert(caps.Size==sizeof(caps)&&caps.Version==NVX_3D_ABI_VERSION);
    assert(caps.GraphicsClass==0xC797&&caps.PageBytes==NVX_3D_PAGE_BYTES);
    assert(caps.Features==0&&caps.MaxPushDwords==NVX_3D_MAX_PUSH_DWORDS&&
        caps.MaxBufferBytes==NVX_3D_MAX_BUFFER_BYTES);
    Nvx3dBuildCaps(&caps,1,1,0,0,1,1,1,1);
    assert(caps.Features==(NVX_3D_FEATURE_GR_CHANNEL|
        NVX_3D_FEATURE_I2M_VERIFIED|NVX_3D_FEATURE_CLEAR_VERIFIED|
        NVX_3D_FEATURE_USER_ALLOC|NVX_3D_FEATURE_SUBMIT));
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
    alloc.Usage=3;alloc.Bytes=65536;
    assert(Nvx3dValidateAlloc(&alloc));
    assert(Nvx3dDispatch(NVX_3D_OP_ALLOC,&alloc,sizeof(alloc),NULL,0,NULL,
        0,0,0,0,0,0)==NVX_3D_STATUS_SUCCESS);
    alloc.Bytes=65535;assert(!Nvx3dValidateAlloc(&alloc));
    alloc.Bytes=NVX_3D_MAX_BUFFER_BYTES+4096;assert(!Nvx3dValidateAlloc(&alloc));
    alloc.Bytes=4096;alloc.BufferId=1;assert(!Nvx3dValidateAlloc(&alloc));

    submit.Size=sizeof(submit);submit.Version=NVX_3D_ABI_VERSION;
    submit.Operation=NVX_3D_SUBMIT_CLEAR_SURFACE;
    submit.Width=64;submit.Height=16;submit.BufferId=1;
    assert(Nvx3dValidateSubmit(&submit,sizeof(submit),65536));
    assert(Nvx3dDispatch(NVX_3D_OP_SUBMIT,&submit,sizeof(submit),NULL,0,NULL,
        0,0,0,0,0,0)==NVX_3D_STATUS_SUCCESS);
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),4095));
    submit.Width=65;assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),65536));
    submit.Width=64;submit.Reserved[0]=1;
    assert(!Nvx3dValidateSubmit(&submit,sizeof(submit),65536));
    submit.Reserved[0]=0;

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

    puts("3D ABI layout, capabilities, ownership and command checks passed");
    return 0;
}
