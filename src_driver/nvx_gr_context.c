#include "nvx_gr_context.h"
#include <limits.h>

int NvxGrBuildPtes(uint64_t *entries, uint32_t capacity, uint64_t physical, uint64_t bytes)
{
    uint64_t pages, i;
    if(!entries||!bytes||(physical&4095)||(bytes&4095)||
       physical>UINT64_MAX-bytes) return 0;
    pages=bytes>>12;
    if(pages>capacity||physical+bytes>UINT64_C(0x100000000)) return 0;
    for(i=0;i<pages;i++)
        /* Pascal VER2 PTE: valid, SYS_COH, volatile, privileged. */
        entries[i]=((physical+(i<<12))>>4)|UINT64_C(0x2d);
    return 1;
}

int NvxGrBuildLayout(const NVX_GR_REQUIREMENT *req, uint32_t subcontexts,
                     int perSubcontextHeader, uint64_t capacity, NVX_GR_LAYOUT *out)
{
    /* MAIN, PM, PATCH, BUNDLE, PAGEPOOL, ATTRIBUTE, RTV, GFXP pool/control,
       FECS event, and separate restricted/unrestricted privilege maps. */
    static const struct { uint8_t index, id, required; } spec[] = {
        {0,0,1},{9,1,0},{16,2,1},{17,3,1},{18,4,1},{19,5,1},
        {20,6,0},{21,7,0},{22,8,0},{23,9,0},{24,10,0},{24,11,0}
    };
    NVX_GR_LAYOUT plan={0};
    unsigned i;
    if(!req||!out||!capacity||(perSubcontextHeader!=0&&perSubcontextHeader!=1)||
       (perSubcontextHeader&&(!subcontexts||subcontexts>4096))) return 0;
    for(i=0;i<sizeof(spec)/sizeof(spec[0]);i++) {
        uint64_t size=req[spec[i].index].size;
        uint64_t align=req[spec[i].index].alignment;
        uint64_t offset,add;
        if(!size||size==UINT32_MAX) { if(spec[i].required) return 0; else continue; }
        if(!align||align==UINT32_MAX||(align&(align-1))||align>0x200000U) return 0;
        if(align<4096) align=4096;
        if(spec[i].id==0&&perSubcontextHeader) {
            size=(size+4095U)&~UINT64_C(4095);
            add=(uint64_t)subcontexts*4096U;
            if(size>UINT64_MAX-add) return 0;
            size+=add;
        }
        if(plan.bytes>UINT64_MAX-(align-1)) return 0;
        offset=(plan.bytes+align-1)&~(align-1);
        if(offset>capacity||size>capacity-offset) return 0;
        plan.buffers[plan.count].id=spec[i].id;
        plan.buffers[plan.count].alignment=(uint32_t)align;
        plan.buffers[plan.count].offset=offset;
        plan.buffers[plan.count].size=size;
        plan.count++;
        plan.bytes=offset+size;
    }
    if(plan.bytes>UINT64_MAX-4095) return 0;
    plan.bytes=(plan.bytes+4095)&~UINT64_C(4095);
    if(plan.bytes>capacity) return 0;
    *out=plan;
    return 1;
}
