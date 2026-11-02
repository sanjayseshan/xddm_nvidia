#include "nvx_seq.h"
unsigned NvxSeqArgs(uint32_t op)
{
    static const unsigned args[9]={2,3,5,1,2,0,0,0,0};
    return op<9?args[op]:~0U;
}
int NvxValidateSeq(const uint32_t *p,size_t bytes,uint32_t barLength,uint32_t *commands)
{
    uint32_t used,capacity,index=0,n=0;
    if(!p||!commands||bytes<40||barLength<4) return 0;
    capacity=p[0];used=p[1];
    if(!capacity||capacity>16384||used>=capacity||used>(bytes-40)/4) return 0;
    while(index<used) {
        uint32_t op=p[10+index++];unsigned args=NvxSeqArgs(op);
        if(args==~0U||args>used-index) return 0;
        if(op<=2||op==4) {
            uint32_t addr=p[10+index];
            if((addr&3)||addr>barLength-4) return 0;
        }
        if(op==4&&p[10+index+1]>=8) return 0;
        if(op==2&&p[10+index+3]>2000000U) return 0;
        if(op==3&&p[10+index]>2000000U) return 0;
        index+=args;n++;
    }
    *commands=n;return 1;
}
