/* Checked readers for NVIDIA 535.113.01 firmware containers. */
#include "nvx_fw.h"

static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t u64(const unsigned char *p)
{
    return u32(p) | (uint64_t)u32(p + 4) << 32;
}
static int range(size_t size, uint64_t off, uint64_t len)
{
    return off <= size && len <= size - off;
}
int NvxParseBooter(const unsigned char *p, size_t size, NVX_BOOTER_INFO *b)
{
    uint32_t h, load, meta, loc, sig, count, prod, prodsize;
    if (!p || !b || size < 24 || u32(p) != 0x10de || u32(p+4) != 1) return 0;
    h = u32(p+12);
    if (!range(size,h,36)) return 0;
    b->DataOffset = u32(p+16); b->DataSize = u32(p+20);
    if (!range(size,b->DataOffset,b->DataSize)) return 0;
    prod = u32(p+h); prodsize = u32(p+h+4);
    loc = u32(p+h+8); sig = u32(p+h+12); meta = u32(p+h+16);
    count = u32(p+h+24); load = u32(p+h+28);
    if (!range(size,loc,4) || !range(size,sig,4) || !range(size,count,4) ||
        !range(size,meta,12) || !range(size,load,36) ||
        !range(size,prod,prodsize)) return 0;
    b->SignatureCount = u32(p+count);
    if (!b->SignatureCount || prodsize % b->SignatureCount) return 0;
    b->SignatureSize = prodsize / b->SignatureCount;
    if (!b->SignatureSize || u32(p+sig) > prodsize ||
        (uint64_t)u32(p+sig) + (uint64_t)b->SignatureCount*b->SignatureSize > prodsize)
        return 0;
    b->SignatureOffset = prod + u32(p+sig);
    b->PatchOffset = u32(p+loc);
    b->ImemOffset = u32(p+load+20); b->ImemSize = u32(p+load+24);
    b->DmemOffset = u32(p+load+8); b->DmemSize = u32(p+load+12);
    b->FuseVersion = u32(p+meta); b->EngineId = u32(p+meta+4); b->UcodeId = u32(p+meta+8);
    return u32(p+load+16) == 1 && b->UcodeId &&
           b->ImemSize && b->DmemSize &&
           !((b->ImemOffset | b->ImemSize | b->DmemOffset | b->DmemSize) & 255) &&
           range(b->DataSize,b->ImemOffset,b->ImemSize) &&
           range(b->DataSize,b->DmemOffset,b->DmemSize) &&
           b->PatchOffset >= b->DmemOffset &&
           range((uint64_t)b->DmemOffset+b->DmemSize,b->PatchOffset,b->SignatureSize);
}
int NvxSelectSignature(uint32_t fuse, uint32_t version, uint32_t count)
{
    uint32_t bits = 0, index;
    if (!count) return -1;
    if (!fuse) return (int)(count - 1);
    while (fuse) { bits++; fuse >>= 1; }
    if (bits > version) return -1;
    index = version - bits;
    return index < count ? (int)index : -1;
}
int NvxFindElfSection(const unsigned char *p, size_t size, const char *name,
                      uint32_t *offset, uint32_t *length)
{
    uint64_t sh, names, namesize, pos, off, len;
    uint32_t n, i, stridx, nameoff;
    if (!p || !name || size < 64 || u32(p) != 0x464c457f || p[4] != 2 || p[5] != 1)
        return 0;
    sh = u64(p+40); n = p[60] | p[61]<<8; stridx = p[62] | p[63]<<8;
    if ((p[58] | p[59]<<8) != 64 || stridx >= n || !range(size,sh,(uint64_t)n*64))
        return 0;
    names = u64(p+sh+stridx*64+24); namesize = u64(p+sh+stridx*64+32);
    if (!range(size,names,namesize)) return 0;
    for (i=0;i<n;i++) {
        const char *s = name;
        pos = sh+(uint64_t)i*64; nameoff = u32(p+pos);
        if (nameoff >= namesize) return 0;
        while (nameoff < namesize && *s && p[names+nameoff] == (unsigned char)*s) {
            nameoff++; s++;
        }
        if (*s || nameoff == namesize || p[names+nameoff]) continue;
        off = u64(p+pos+24); len = u64(p+pos+32);
        if (!range(size,off,len) || off > UINT32_MAX || len > UINT32_MAX) return 0;
        *offset = (uint32_t)off; *length = (uint32_t)len;
        return 1;
    }
    return 0;
}
