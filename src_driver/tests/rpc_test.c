#include "nvx_rpc.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    uint8_t slot[4096],payload[8]={8,0,0,0,0,0,0,0};uint32_t f,r,s;
    assert(NvxEncodeRpc(slot,sizeof(slot),73,0,payload,8));
    assert(slot[40]==1&&slot[56]==40&&slot[60]==73&&slot[80]==8);
    assert(NvxDecodeRpc(slot,sizeof(slot),&f,&r,&s));
    assert(f==73&&r==0xffffffffU&&s==0);
    slot[101]^=1;assert(!NvxDecodeRpc(slot,sizeof(slot),&f,&r,&s));
    assert(!NvxEncodeRpc(slot,4095,73,0,payload,8));
    assert(!NvxEncodeRpc(slot,4096,73,0,NULL,8));
    assert(!NvxEncodeRpc(slot,4096,73,0,payload,4017));
    puts("RPC tests passed: 535 frame, checksum, corruption and bounds.");return 0;
}
