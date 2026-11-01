/* Bounded, read-only parser derived from the OpenRM 535 VBIOS/FWSEC layout. */
#include "nvx_vbios.h"
#include <stddef.h>

typedef struct _READER {
    NVX_ROM_READ32 Read32;
    void *Context;
    uint32_t Limit;
    uint32_t CachedOffset;
    uint32_t CachedValue;
    int CacheValid;
} READER;

static void zero_bytes(void *ptr, size_t size)
{
    volatile unsigned char *p=(volatile unsigned char *)ptr;
    while (size--) *p++=0;
}

static int rd32(READER *r, uint32_t off, uint32_t *v)
{
    if (!r || !v || off > r->Limit || r->Limit - off < 4) return 0;
    if (!r->CacheValid || r->CachedOffset != off) {
        if (!r->Read32(r->Context, off, &r->CachedValue)) return 0;
        r->CachedOffset = off;
        r->CacheValid = 1;
    }
    *v = r->CachedValue;
    return 1;
}

static int rd8(READER *r, uint32_t off, uint8_t *v)
{
    uint32_t d, aligned = off & ~3U;
    if (!v || !rd32(r, aligned, &d)) return 0;
    *v = (uint8_t)(d >> ((off & 3U) * 8));
    return 1;
}

static int rd16(READER *r, uint32_t off, uint16_t *v)
{
    uint8_t a, b;
    if (!v || !rd8(r, off, &a) || !rd8(r, off + 1, &b)) return 0;
    *v = (uint16_t)a | (uint16_t)b << 8;
    return 1;
}

static int u32at(READER *r, uint32_t off, uint32_t *v)
{
    uint8_t b0, b1, b2, b3;
    if (!rd8(r,off,&b0) || !rd8(r,off+1,&b1) ||
        !rd8(r,off+2,&b2) || !rd8(r,off+3,&b3)) return 0;
    *v = (uint32_t)b0 | (uint32_t)b1 << 8 |
         (uint32_t)b2 << 16 | (uint32_t)b3 << 24;
    return 1;
}

static int add(uint32_t a, uint32_t b, uint32_t limit, uint32_t *out)
{
    if (a > limit || b > limit - a) return 0;
    *out = a + b;
    return 1;
}

static int locate_roms(READER *r, uint32_t *biosSize, uint32_t *expansion)
{
    uint32_t block = 0, baseSize = 0, extOffset = 0, end = 0;
    unsigned image;
    for (image = 0; image < 16; image++) {
        uint16_t sig, pcirPtr, imageBlocks, pcirLen, npdeRev, npdeLen, subBlocks;
        uint32_t pcir, pcirSig, npde, npdeSig, blockSize;
        uint8_t type, indicator, npdeLast;
        int last;
        if (!rd16(r, block, &sig) || sig != 0xaa55 ||
            !rd16(r, block + 0x18, &pcirPtr) ||
            !add(block, pcirPtr, r->Limit, &pcir) ||
            !u32at(r, pcir, &pcirSig) || pcirSig != 0x52494350 ||
            !rd16(r, pcir + 0x0a, &pcirLen) ||
            !rd16(r, pcir + 0x10, &imageBlocks) || !imageBlocks ||
            !rd8(r, pcir + 0x14, &type) ||
            !rd8(r, pcir + 0x15, &indicator)) return 0;
        subBlocks = imageBlocks;
        last = (indicator & 0x80) != 0;
        npde = (pcir + pcirLen + 15U) & ~15U;
        if (u32at(r, npde, &npdeSig) && npdeSig == 0x4544504e &&
            rd16(r, npde + 4, &npdeRev) &&
            (npdeRev == 0x0100 || npdeRev == 0x0101) &&
            rd16(r, npde + 6, &npdeLen) && rd16(r, npde + 8, &subBlocks) &&
            subBlocks) {
            if (npdeLen > 0x0a && rd8(r, npde + 0x0a, &npdeLast))
                last = (npdeLast & 0x80) != 0;
        }
        blockSize = (uint32_t)subBlocks * 512U;
        if (!add(block, blockSize, r->Limit, &end)) return 0;
        if (!baseSize && type == 0x00) baseSize = blockSize;
        if (!extOffset && type == 0xe0) extOffset = block;
        if (last) break;
        block = end;
    }
    if (image == 16 || !end) return 0;
    *biosSize = end;
    *expansion = (extOffset && baseSize && extOffset >= baseSize) ?
                 extOffset - baseSize : 0;
    return 1;
}

static int find_bit(READER *r, uint32_t biosSize, uint32_t *bit)
{
    uint32_t off;
    for (off = 0; off + 12 <= biosSize; off++) {
        uint16_t id;
        uint32_t sig, sum = 0, i;
        uint8_t size, b;
        if (!rd16(r,off,&id) || id != 0xb8ff ||
            !u32at(r,off+2,&sig) || sig != 0x00544942 ||
            !rd8(r,off+8,&size) || size < 12 || off > biosSize - size)
            continue;
        for (i=0; i<size; i++) { if (!rd8(r,off+i,&b)) return 0; sum += b; }
        if (!(sum & 0xff)) { *bit = off; return 1; }
    }
    return 0;
}

int NvxParseFwsecRom(NVX_ROM_READ32 read32, void *context,
                     uint32_t maxSize, NVX_FWSEC_INFO *info)
{
    READER r;
    uint32_t bit=0, tokenBase, tableRel = 0, table, descRel, desc, vdesc;
    uint32_t i, imageEnd;
    uint8_t headerSize, tokenSize, tokenCount;
    uint8_t tableVersion, tableHeader, entrySize, entryCount;
    if (!read32 || !info || maxSize < 512) return NVX_VBIOS_IO_ERROR;
    zero_bytes(info,sizeof(*info)); zero_bytes(&r,sizeof(r));
    r.Read32=read32; r.Context=context; r.Limit=maxSize;
    if (!locate_roms(&r,&info->BiosSize,&info->ExpansionRomOffset))
        return NVX_VBIOS_INVALID_ROM;
    if (!find_bit(&r,info->BiosSize,&bit)) return NVX_VBIOS_NO_BIT;
    info->BitOffset=bit;
    if (!rd8(&r,bit+8,&headerSize) || !rd8(&r,bit+9,&tokenSize) ||
        !rd8(&r,bit+10,&tokenCount) || tokenSize < 6 ||
        !add(bit,headerSize,info->BiosSize,&tokenBase)) return NVX_VBIOS_NO_BIT;
    for (i=0; i<tokenCount; i++) {
        uint32_t tok; uint8_t id, ver; uint16_t size, ptr;
        if (i > (info->BiosSize-tokenBase)/tokenSize) return NVX_VBIOS_NO_BIT;
        tok=tokenBase+i*tokenSize;
        if (!rd8(&r,tok,&id)||!rd8(&r,tok+1,&ver)||!rd16(&r,tok+2,&size)||!rd16(&r,tok+4,&ptr))
            return NVX_VBIOS_IO_ERROR;
        if (id==0x42 && (ver==1||ver==2) && size>=5) {
            uint32_t version; uint8_t oem;
            if (u32at(&r,ptr,&version)&&rd8(&r,ptr+4,&oem))
                info->VbiosVersion=((uint64_t)version<<8)|oem;
        }
        if (id==0x70 && ver==2 && size>=4 && u32at(&r,ptr,&tableRel)) break;
    }
    if (!tableRel || !add(info->ExpansionRomOffset,tableRel,info->BiosSize,&table))
        return NVX_VBIOS_NO_FWSEC;
    info->FalconTableOffset=table;
    if (!rd8(&r,table,&tableVersion)||!rd8(&r,table+1,&tableHeader)||
        !rd8(&r,table+2,&entrySize)||!rd8(&r,table+3,&entryCount)||
        tableVersion!=1||tableHeader<6||entrySize<6) return NVX_VBIOS_NO_FWSEC;
    for (i=0; i<entryCount; i++) {
        uint32_t entry; uint8_t app,target;
        if (i > (info->BiosSize-table-tableHeader)/entrySize) return NVX_VBIOS_NO_FWSEC;
        entry=table+tableHeader+i*entrySize;
        if (!rd8(&r,entry,&app)||!rd8(&r,entry+1,&target)||!u32at(&r,entry+2,&descRel))
            return NVX_VBIOS_IO_ERROR;
        if (app!=0x05 && app!=0x85) continue;
        if (!add(info->ExpansionRomOffset,descRel,info->BiosSize,&desc)||
            !u32at(&r,desc,&vdesc)) return NVX_VBIOS_BAD_DESCRIPTOR;
        info->DescriptorVersion=(uint8_t)(vdesc>>8);
        info->DescriptorSize=vdesc>>16;
        if (!(vdesc&1) ||
            !((info->DescriptorVersion==2&&info->DescriptorSize>=60)||
              (info->DescriptorVersion==3&&info->DescriptorSize>=44)) ||
            !add(desc,info->DescriptorSize,info->BiosSize,&info->ImageOffset))
            return NVX_VBIOS_BAD_DESCRIPTOR;
        info->DescriptorOffset=desc; info->ApplicationId=app; info->TargetId=target;
        if (!u32at(&r,desc+4,&info->StoredSize)) return NVX_VBIOS_BAD_DESCRIPTOR;
        if (info->DescriptorVersion==3) {
            if (!u32at(&r,desc+12,&info->InterfaceOffset)||
                !u32at(&r,desc+20,&info->ImemLoadSize)||
                !u32at(&r,desc+32,&info->DmemLoadSize)||
                !rd16(&r,desc+36,&info->EngineIdMask)||
                !rd8(&r,desc+38,&info->UcodeId)||
                !rd8(&r,desc+39,&info->SignatureCount)||
                !rd16(&r,desc+40,&info->SignatureVersions)) return NVX_VBIOS_BAD_DESCRIPTOR;
        } else {
            if (!u32at(&r,desc+16,&info->InterfaceOffset)||
                !u32at(&r,desc+24,&info->ImemLoadSize)||
                !u32at(&r,desc+48,&info->DmemLoadSize)) return NVX_VBIOS_BAD_DESCRIPTOR;
        }
        if (!info->StoredSize || !add(info->ImageOffset,info->StoredSize,info->BiosSize,&imageEnd))
            return NVX_VBIOS_BAD_DESCRIPTOR;
        return NVX_VBIOS_OK;
    }
    return NVX_VBIOS_NO_FWSEC;
}
