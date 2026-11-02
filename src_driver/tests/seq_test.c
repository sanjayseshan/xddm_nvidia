#include "nvx_seq.h"
#include <assert.h>
#include <stdio.h>
static uint32_t regs[128],writes,delays,settle,reads;
static int rd(void *c,uint32_t a,uint32_t *v) {
    (void)c;if(settle && ++reads==2) regs[a/4]=9;*v=regs[a/4];return 1;
}
static int wr(void *c,uint32_t a,uint32_t v) { (void)c;regs[a/4]=v;writes++;return 1; }
static int delay(void *c,uint32_t us) { (void)c;delays+=us;return 1; }
static int core(void *c,uint32_t op) { (void)c;return op==8; }
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
    {
        NVX_SEQ_IO io={NULL,rd,wr,delay,core};uint32_t failed;
        regs[0x104/4]=0x12345678;
        assert(NvxExecuteSeq(p,sizeof(p),sizeof(regs),&io,&failed));
        assert(writes==1&&regs[0x100/4]==5&&p[9]==0x12345678&&delays==10);
        io.core=NULL;writes=0;
        assert(!NvxExecuteSeq(p,sizeof(p),sizeof(regs),&io,&failed)&&writes==0);
        uint32_t poll[16]={16,6,0,0,0,0,0,0,0,0,2,0x100,0xff,9,20,1};
        assert(!NvxExecuteSeq(poll,sizeof(poll),sizeof(regs),&io,&failed));
        assert(failed==0&&delays==30);
        poll[14]=0;settle=1;reads=0;
        assert(NvxExecuteSeq(poll,sizeof(poll),sizeof(regs),&io,&failed));
        assert(delays==40);
    }
    puts("Sequencer tests passed: opcodes, operand bounds, MMIO alignment and save slots.");return 0;
}
