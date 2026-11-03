#include "nvx_mmu.h"
#define ADDRESS_MASK UINT64_C(0x0000ffffffffffff)
int NvxMmuCheckRange(uint64_t base,uint64_t length,uint64_t address,uint64_t bytes)
{
    if(!length||!bytes||address<base) return NVX_MMU_RANGE;
    address-=base;
    if(address>=length||bytes>length-address) return NVX_MMU_RANGE;
    return NVX_MMU_OK;
}
static int read_entry(NVX_MMU_READ64 read,void *ctx,uint64_t bytes,uint64_t addr,uint64_t *entry)
{
    if((addr&7)||bytes<8||addr>bytes-8) return NVX_MMU_RANGE;
    return read(ctx,addr,entry)?NVX_MMU_OK:NVX_MMU_IO;
}
static int table(uint64_t entry,uint64_t bytes,uint64_t *addr)
{
    unsigned aper=(unsigned)(entry>>1)&3;
    if(!aper) return NVX_MMU_ABSENT;
    if(aper!=1) return NVX_MMU_APERTURE;
    *addr=(entry&ADDRESS_MASK&~UINT64_C(15))<<4;
    return *addr<bytes?NVX_MMU_OK:NVX_MMU_RANGE;
}
static int leaf(uint64_t entry,uint64_t va,uint64_t bytes,unsigned shift,NVX_MMU_MAPPING *out)
{
    uint64_t size=UINT64_C(1)<<shift,base;
    unsigned kind=(unsigned)(entry>>56);
    if(!(entry&1)) return NVX_MMU_ABSENT;
    if((entry>>1)&3) return NVX_MMU_APERTURE;
    if(kind!=0&&kind!=6) return NVX_MMU_KIND;
    base=((entry&ADDRESS_MASK&~UINT64_C(255))<<4)&~(size-1);
    if(bytes<size||base>bytes-size) return NVX_MMU_RANGE;
    out->physical=base+(va&(size-1));out->pageShift=shift;
    out->kind=kind;out->writable=!(entry&64);
    return NVX_MMU_OK;
}
int NvxMmuTranslateVram(NVX_MMU_READ64 read,void *ctx,uint64_t pdb,uint64_t va,
                        uint64_t bytes,unsigned bigShift,NVX_MMU_MAPPING *out)
{
    static const unsigned shifts[3]={47,38,29};
    uint64_t entry,big,small,addr=pdb;
    int status;
    if(!out) return NVX_MMU_INVALID;
    out->physical=0;out->pageShift=out->kind=out->writable=0;
    if(!read||!pdb||(pdb&4095)||pdb>=bytes||bytes>(UINT64_C(1)<<52)||(va>>49)||
       (bigShift!=16&&bigShift!=17)) return NVX_MMU_INVALID;
    for(unsigned level=0;level<3;level++) {
        uint64_t index=(va>>shifts[level])&(level?511:3);
        status=read_entry(read,ctx,bytes,addr+index*8,&entry);
        if(status!=NVX_MMU_OK) return status;
        status=table(entry,bytes,&addr);
        if(status!=NVX_MMU_OK) return status;
    }
    addr+=((va>>21)&255)*16;
    status=read_entry(read,ctx,bytes,addr,&big);
    if(status!=NVX_MMU_OK) return status;
    status=read_entry(read,ctx,bytes,addr+8,&small);
    if(status!=NVX_MMU_OK) return status;
    if(big&1) return leaf(big,va,bytes,21,out);
    status=table(big,bytes,&addr);
    if(status==NVX_MMU_OK) {
        status=read_entry(read,ctx,bytes,addr+((va>>bigShift)&((1U<<(21-bigShift))-1))*8,&entry);
        if(status!=NVX_MMU_OK) return status;
        if(entry&1) return leaf(entry,va,bytes,bigShift,out);
        if(entry&(32|8)) return NVX_MMU_ABSENT; /* sparse/nonresident big leaves */
    } else if(status!=NVX_MMU_ABSENT) return status;
    status=table(small,bytes,&addr);
    if(status!=NVX_MMU_OK) return status;
    status=read_entry(read,ctx,bytes,addr+((va>>12)&511)*8,&entry);
    if(status!=NVX_MMU_OK) return status;
    return leaf(entry,va,bytes,12,out);
}
