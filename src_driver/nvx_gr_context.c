#include "nvx_gr_context.h"
#include <limits.h>

int NvxGrBuildPtes(uint64_t *entries, uint32_t capacity, uint64_t physical, uint64_t bytes, int privileged)
{
    uint64_t pages, i;
    if(!entries||!bytes||(privileged!=0&&privileged!=1)||(physical&4095)||(bytes&4095)||
       physical>UINT64_MAX-bytes) return 0;
    pages=bytes>>12;
    if(pages>capacity||physical+bytes>UINT64_C(0x100000000)) return 0;
    for(i=0;i<pages;i++)
        /* Pascal VER2 PTE: valid, SYS_COH, volatile, optional privilege. */
        entries[i]=((physical+(i<<12))>>4)|UINT64_C(0x0d)|(privileged?0x20U:0);
    return 1;
}

static int nvx_gart_range_valid(uint32_t first, uint32_t count)
{
    return count && first < NVX_GART_PAGE_COUNT &&
        count <= NVX_GART_PAGE_COUNT - first;
}

int NvxGartReserve(NVX_GART_HEAP *heap, uint64_t bytes, uint32_t alignment,
                   uint32_t *first,
                   uint32_t *count, uint64_t *gpu_va)
{
    uint32_t needed, alignment_pages, start, i;
    if (first) *first = 0;
    if (count) *count = 0;
    if (gpu_va) *gpu_va = 0;
    if (!heap || !bytes || !first || !count || !gpu_va ||
        bytes > NVX_GART_TOTAL_BYTES ||
        bytes > UINT64_MAX - (NVX_GART_PAGE_BYTES - 1U) ||
        (alignment && (alignment < NVX_GART_PAGE_BYTES ||
                       (alignment & (alignment - 1U)) ||
                       alignment > NVX_GART_SLICE_BYTES)))
        return 0;
    if (!alignment)
        alignment = NVX_GART_PAGE_BYTES;
    needed = (uint32_t)((bytes + NVX_GART_PAGE_BYTES - 1U) /
                       NVX_GART_PAGE_BYTES);
    alignment_pages = alignment / NVX_GART_PAGE_BYTES;
    for (start = 0; nvx_gart_range_valid(start, needed); start++) {
        uint32_t remainder = start % alignment_pages;
        if (remainder) {
            start += alignment_pages - remainder - 1U;
            continue;
        }
        for (i = 0; i < needed; i++) {
            uint32_t bit = start + i;
            if (heap->used[bit / 32U] & (1U << (bit % 32U)))
                break;
        }
        if (i != needed) {
            start += i;
            continue;
        }
        for (i = 0; i < needed; i++) {
            uint32_t bit = start + i;
            heap->used[bit / 32U] |= 1U << (bit % 32U);
        }
        *first = start;
        *count = needed;
        *gpu_va = NVX_GART_GPU_VA_BASE +
                  (uint64_t)start * NVX_GART_PAGE_BYTES;
        return 1;
    }
    return 0;
}

int NvxGartRelease(NVX_GART_HEAP *heap, uint32_t first, uint32_t count)
{
    uint32_t i;
    if (!heap || !nvx_gart_range_valid(first, count))
        return 0;
    for (i = 0; i < count; i++) {
        uint32_t bit = first + i;
        if (!(heap->used[bit / 32U] & (1U << (bit % 32U))) )
            return 0;
    }
    for (i = 0; i < count; i++) {
        uint32_t bit = first + i;
        heap->used[bit / 32U] &= ~(1U << (bit % 32U));
    }
    return 1;
}

int NvxGartBuildPtes(uint64_t *entries, uint32_t capacity,
                     uint32_t first_page, uint32_t page_count,
                     uint64_t physical, uint64_t bytes, int privileged)
{
    uint64_t max_bytes;
    if (!entries || !nvx_gart_range_valid(first_page, page_count) ||
        !bytes || (bytes & (NVX_GART_PAGE_BYTES - 1U)) ||
        capacity < NVX_GART_SLICE_COUNT * NVX_GART_PTES_PER_SLICE)
        return 0;
    max_bytes = (uint64_t)page_count * NVX_GART_PAGE_BYTES;
    if (bytes > max_bytes)
        return 0;
    return NvxGrBuildPtes(entries + first_page, page_count, physical,
                          bytes, privileged);
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
