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
    {
        static uint32_t record[2048];uint32_t elements;
        assert(NvxEncodeRpc((uint8_t *)record,sizeof(record),0x100c,9,NULL,0));
        record[10]=2;record[14]=5000;record[1024]=0x12345678;
        record[8]=0;
        for(unsigned i=0;i<2048;i++) if(i!=8) record[8]^=record[i];
        assert(NvxDecodeRpcRecord((uint8_t *)record,sizeof(record),&f,&r,&s,&elements));
        assert(elements==2&&s==9);
        assert(!NvxDecodeRpcRecord((uint8_t *)record,4096,&f,&r,&s,&elements));
        record[1024]^=1;
        assert(!NvxDecodeRpcRecord((uint8_t *)record,sizeof(record),&f,&r,&s,&elements));
    }
    puts("RPC tests passed: 535 frame, checksum, corruption and bounds.");return 0;
}
