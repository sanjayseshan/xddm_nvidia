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
int NvxExecuteSeq(uint32_t *p,size_t bytes,uint32_t barLength,const NVX_SEQ_IO *io,
                  uint32_t *failedWord)
{
    uint32_t count,index=0,budget=2000000;
    if(!failedWord) return 0;
    *failedWord=0;
    if(!io||!io->read||!io->write||!io->delay||!NvxValidateSeq(p,bytes,barLength,&count)) return 0;
    /* Reject unsupported transitions before making any register changes. */
    while(index<p[1]) {
        uint32_t op=p[10+index];
        if(op>=5&&!io->core) { *failedWord=index;return 0; }
        index+=1+NvxSeqArgs(op);
    }
    index=0;
    while(index<p[1]) {
        uint32_t start=index,op=p[10+index++],*a=p+10+index,value=0;
        int ok=1;
        *failedWord=start;
        switch(op) {
        case 0: ok=io->write(io->context,a[0],a[1]);break;
        case 1:
            ok=io->read(io->context,a[0],&value);
            if(ok) ok=io->write(io->context,a[0],(value&~a[1])|a[2]);
            break;
        case 2: {
            /* OpenRM zero selects the default timeout, not no waiting. */
            uint32_t remaining=a[3]?a[3]:2000000;
            for(;;) {
                ok=io->read(io->context,a[0],&value);
                if(!ok||(value&a[1])==a[2]) break;
                if(!remaining||!budget) { ok=0;break; }
                uint32_t step=remaining<10?remaining:10;
                if(step>budget) step=budget;
                ok=io->delay(io->context,step);
                remaining-=step;budget-=step;
                if(!ok) break;
            }
            break;
        }
        case 3:
            if(a[0]>budget) ok=0;
            else { budget-=a[0];ok=io->delay(io->context,a[0]); }break;
        case 4:
            ok=io->read(io->context,a[0],&value);
            if(ok) p[2+a[1]]=value;
            break;
        default: ok=io->core(io->context,op);break;
        }
        if(!ok) return 0;
        index+=NvxSeqArgs(op);
    }
    *failedWord=p[1];return 1;
}
