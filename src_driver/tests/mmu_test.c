#include "nvx_mmu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint64_t mem[0x10000/8];
static int reads;
static int read64(void *ctx,uint64_t addr,uint64_t *value)
{
    (void)ctx;reads++;
    if(addr>=sizeof(mem)||(addr&7)) return 0;
    *value=mem[addr/8];return 1;
}
static uint64_t ptr(uint64_t addr) {return (addr>>4)|2;}
static uint64_t pte(uint64_t addr) {return (addr>>4)|1;}
int main(void)
{
    NVX_MMU_MAPPING m;
    assert(NvxMmuCheckRange(0x20000000,0x2000000,0x20000000,4)==NVX_MMU_OK);
    assert(NvxMmuCheckRange(0x20000000,0x2000000,0x21ffffff,1)==NVX_MMU_OK);
    assert(NvxMmuCheckRange(0x20000000,0x2000000,0x21ffffff,2)==NVX_MMU_RANGE);
    assert(NvxMmuCheckRange(0x20000000,0x2000000,0x1fffffff,4)==NVX_MMU_RANGE);
    uint64_t va=0x246789,bytes=0x20000000;
    memset(mem,0,sizeof(mem));
    mem[0x1000/8]=ptr(0x2000);mem[0x2000/8]=ptr(0x3000);mem[0x3000/8]=ptr(0x4000);
    mem[0x4018/8]=ptr(0x6000);mem[(0x6000+0x46*8)/8]=pte(0x11000000);
    assert(NvxMmuTranslateVram(read64,0,0x1000,va,bytes,16,&m)==NVX_MMU_OK);
    assert(m.physical==0x11000789&&m.pageShift==12&&m.writable);
    mem[0x4010/8]=ptr(0x5000);mem[(0x5000+4*8)/8]=pte(0x12000000);
    assert(NvxMmuTranslateVram(read64,0,0x1000,va,bytes,16,&m)==NVX_MMU_OK);
    assert(m.physical==0x12006789&&m.pageShift==16);
    mem[(0x5000+2*8)/8]=pte(0x14000000);
    assert(NvxMmuTranslateVram(read64,0,0x1000,va,bytes,17,&m)==NVX_MMU_OK);
    assert(m.physical==0x14006789&&m.pageShift==17);
    mem[(0x5000+4*8)/8]=32;
    assert(NvxMmuTranslateVram(read64,0,0x1000,va,bytes,16,&m)==NVX_MMU_ABSENT);
    mem[(0x5000+4*8)/8]=0;
    assert(NvxMmuTranslateVram(read64,0,0x1000,va,bytes,16,&m)==NVX_MMU_OK&&m.pageShift==12);
    mem[0x4010/8]=pte(0x10000000)|64;
    assert(NvxMmuTranslateVram(read64,0,0x1000,va,bytes,16,&m)==NVX_MMU_OK);
    assert(m.physical==0x10046789&&m.pageShift==21&&!m.writable);
    mem[0x4010/8]=pte(0x10000000)|(UINT64_C(6)<<56);
    assert(NvxMmuTranslateVram(read64,0,0x1000,va,bytes,16,&m)==NVX_MMU_OK&&m.kind==6);
    mem[0x4010/8]|=UINT64_C(8)<<56;
    assert(NvxMmuTranslateVram(read64,0,0x1000,va,bytes,16,&m)==NVX_MMU_KIND);
    mem[0x4010/8]=pte(0x10000000)|4;
    assert(NvxMmuTranslateVram(read64,0,0x1000,va,bytes,16,&m)==NVX_MMU_APERTURE);
    mem[0x4010/8]=pte(0x20000000);
    assert(NvxMmuTranslateVram(read64,0,0x1000,va,bytes,16,&m)==NVX_MMU_RANGE);
    mem[0x1000/8]=ptr(0x2000)|4;
    assert(NvxMmuTranslateVram(read64,0,0x1000,va,bytes,16,&m)==NVX_MMU_APERTURE);
    mem[0x1000/8]=ptr(0x10000);
    assert(NvxMmuTranslateVram(read64,0,0x1000,va,bytes,16,&m)==NVX_MMU_IO);
    reads=0;
    assert(NvxMmuTranslateVram(read64,0,0x1001,va,bytes,16,&m)==NVX_MMU_INVALID&&!reads);
    assert(NvxMmuTranslateVram(read64,0,0x1000,UINT64_C(1)<<49,bytes,16,&m)==NVX_MMU_INVALID&&!reads);
    assert(NvxMmuTranslateVram(read64,0,0x1000,va,bytes,15,&m)==NVX_MMU_INVALID&&!reads);
    puts("MMU tests passed: 4KB/64KB/128KB/2MB, sparse priority, aperture/kind, write protection and bounds.");
}
