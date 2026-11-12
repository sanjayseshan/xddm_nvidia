#include "nvx_gr_context.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    NVX_GR_REQUIREMENT req[NVX_GR_REQUIREMENT_COUNT]={0};
    NVX_GR_LAYOUT plan;
    uint64_t ptes[4]={0};
    uint64_t *gart_ptes;
    NVX_GART_HEAP gart={0};
    uint32_t first,count;
    uint64_t gpu_va;
    const unsigned ids[]={0,9,16,17,18,19,20,21,22,23,24};
    const unsigned sizes[]={0xfb000,0xe000,0x5000,0x3000,0x20000,0x851200,
                            0x80000,0,0x464,0x10000,0x80000};
    unsigned i;
    for(i=0;i<sizeof(ids)/sizeof(ids[0]);i++) {
        req[ids[i]].size=sizes[i]; req[ids[i]].alignment=0x1000;
    }
    assert(NvxGrBuildLayout(req,64,1,0x1000000,&plan));
    assert(plan.count==11&&plan.buffers[0].size==0x13b000);
    assert(plan.buffers[9].id==10&&plan.buffers[10].id==11);
    for(i=0;i<plan.count;i++) {
        assert((plan.buffers[i].offset&4095)==0);
        if(i) assert(plan.buffers[i].offset>=plan.buffers[i-1].offset+plan.buffers[i-1].size);
        assert(plan.buffers[i].offset+plan.buffers[i].size<=plan.bytes);
    }
    assert(!NvxGrBuildLayout(req,64,1,plan.bytes-1,&plan));
    assert(!NvxGrBuildLayout(req,0,1,0x1000000,&plan));
    req[16].size=UINT32_MAX;
    assert(!NvxGrBuildLayout(req,64,1,0x1000000,&plan));
    req[16].size=0x5000; req[19].alignment=3;
    assert(!NvxGrBuildLayout(req,64,1,0x1000000,&plan));
    assert(NvxGrBuildPtes(ptes,4,0x123000,0x3000,1));
    assert(ptes[0]==((UINT64_C(0x123000)>>4)|0x2d));
    assert(ptes[2]==((UINT64_C(0x125000)>>4)|0x2d)&&ptes[3]==0);
    assert(NvxGrBuildPtes(ptes,4,0x123000,0x1000,0));
    assert(ptes[0]==((UINT64_C(0x123000)>>4)|0x0d));
    assert(!NvxGrBuildPtes(ptes,4,0x123000,0x1000,2));
    assert(!NvxGrBuildPtes(ptes,2,0x123000,0x3000,1));
    assert(!NvxGrBuildPtes(ptes,4,0x123001,0x1000,1));
    assert(!NvxGrBuildPtes(ptes,4,UINT64_C(0xfffff000),0x2000,1));
    gart_ptes=(uint64_t *)calloc(NVX_GART_SLICE_COUNT *
                                 NVX_GART_PTES_PER_SLICE,sizeof(uint64_t));
    assert(gart_ptes);
    assert(NvxGartReserve(&gart,0x210000,&first,&count,&gpu_va));
    assert(first==0&&count==2&&gpu_va==NVX_GART_GPU_VA_BASE);
    assert(NvxGartBuildPtes(gart_ptes,
        NVX_GART_SLICE_COUNT*NVX_GART_PTES_PER_SLICE,first,count,
        0x123000,0x210000,0));
    assert(gart_ptes[0]==((UINT64_C(0x123000)>>4)|0x0d));
    assert(gart_ptes[(0x20f000>>12)]==
           ((UINT64_C(0x332000)>>4)|0x0d));
    assert(gart_ptes[(uint32_t)count*NVX_GART_PTES_PER_SLICE-1U]==0);
    assert(!NvxGartRelease(&gart,0,3));
    assert(NvxGartReserve(&gart,1,&first,&count,&gpu_va));
    assert(first==2&&count==1&&gpu_va==NVX_GART_GPU_VA_BASE+0x400000);
    assert(NvxGartRelease(&gart,0,2));
    assert(NvxGartReserve(&gart,0x400000,&first,&count,&gpu_va));
    assert(first==0&&count==2);
    assert(NvxGartRelease(&gart,0,2));
    assert(NvxGartRelease(&gart,2,1));
    assert(!NvxGartReserve(&gart,0,&first,&count,&gpu_va));
    assert(!NvxGartReserve(&gart,
        (uint64_t)NVX_GART_SLICE_COUNT*NVX_GART_SLICE_BYTES+1,
        &first,&count,&gpu_va));
    assert(!NvxGartBuildPtes(gart_ptes,
        NVX_GART_SLICE_COUNT*NVX_GART_PTES_PER_SLICE,0,1,
        0x123000,0x200001,0));
    free(gart_ptes);
    puts("GR context layout tests passed: bounds, alignment, required buffers and subcontext headers.");
    return 0;
}
