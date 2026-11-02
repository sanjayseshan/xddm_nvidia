#include "nvx_seq.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    uint32_t p[32]={32,9,0,0,0,0,0,0,0,0,0,0x100,5,4,0x104,7,3,10,8};
    uint32_t count;
    assert(NvxValidateSeq(p,sizeof(p),0x1000000,&count)&&count==4);
    p[15]=8;assert(!NvxValidateSeq(p,sizeof(p),0x1000000,&count));p[15]=7;
    p[11]=3;assert(!NvxValidateSeq(p,sizeof(p),0x1000000,&count));p[11]=0x100;
    p[18]=9;assert(!NvxValidateSeq(p,sizeof(p),0x1000000,&count));p[18]=8;
    p[1]=32;assert(!NvxValidateSeq(p,sizeof(p),0x1000000,&count));p[1]=9;
    assert(!NvxValidateSeq(p,44,0x1000000,&count));
    puts("Sequencer tests passed: opcodes, operand bounds, MMIO alignment and save slots.");return 0;
}
