#include "nvx_vbios.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned char *p; size_t n; } MEM;
static int read32(void *ctx, uint32_t off, uint32_t *v)
{
    MEM *m=ctx;
    if (off > m->n || m->n-off < 4) return 0;
    *v=(uint32_t)m->p[off]|(uint32_t)m->p[off+1]<<8|
       (uint32_t)m->p[off+2]<<16|(uint32_t)m->p[off+3]<<24;
    return 1;
}
static void p16(unsigned char *p,uint32_t o,uint16_t v){p[o]=v;p[o+1]=v>>8;}
static void p32(unsigned char *p,uint32_t o,uint32_t v){p16(p,o,v);p16(p,o+2,v>>16);}
static void image_ex(unsigned char *p,uint32_t o,uint16_t blocks,unsigned char type,
                     unsigned char last,uint16_t romSig,uint32_t dataSig)
{
    p16(p,o,romSig);p16(p,o+0x18,0x20);p32(p,o+0x20,dataSig);
    p16(p,o+0x2a,0x18);p16(p,o+0x30,blocks);p[o+0x34]=type;p[o+0x35]=last;
}
static void image(unsigned char *p,uint32_t o,uint16_t blocks,unsigned char type,unsigned char last)
{
    image_ex(p,o,blocks,type,last,0xaa55,0x52494350);
}
static void bit_header(unsigned char *p,uint32_t bit,uint16_t pointer,uint32_t tableRel)
{
    unsigned i,sum=0;
    p16(p,bit,0xb8ff);p32(p,bit+2,0x00544942);p16(p,bit+6,0x0100);
    p[bit+8]=12;p[bit+9]=6;p[bit+10]=1;
    for(i=0;i<11;i++) sum+=p[bit+i];
    p[bit+11]=(unsigned char)(0-sum);
    p[bit+12]=0x70;p[bit+13]=2;p16(p,bit+14,4);p16(p,bit+16,pointer);
    p32(p,pointer,tableRel);
}
static void fwsec_v3(unsigned char *p,uint32_t table,uint32_t desc,uint32_t descRel)
{
    p[table]=1;p[table+1]=6;p[table+2]=6;p[table+3]=1;p[table+4]=3;p[table+5]=44;
    p[table+6]=0x85;p[table+7]=1;p32(p,table+8,descRel);
    p32(p,desc,0x01ac0301);p32(p,desc+4,0x500);p32(p,desc+8,0x280);
    p32(p,desc+12,0x10);p32(p,desc+16,0);p32(p,desc+20,0x100);
    p32(p,desc+24,0);p32(p,desc+28,0);p32(p,desc+32,0x400);
    p16(p,desc+36,1);p[desc+38]=3;p[desc+39]=1;p16(p,desc+40,1);
    {
        uint32_t image=desc+0x1ac,dmem=image+0x100,iface=dmem+0x10,mapper=dmem+0x30;
        p[iface]=1;p[iface+1]=4;p[iface+2]=8;p[iface+3]=2;
        p32(p,iface+4,1);p32(p,iface+8,0);
        p32(p,iface+12,4);p32(p,iface+16,0x30);
        p32(p,mapper,0xfaceb00c);p16(p,mapper+4,3);p16(p,mapper+6,64);
        p32(p,mapper+8,0x100);p32(p,mapper+12,0x40);
        p32(p,mapper+16,0x140);p32(p,mapper+20,0x40);
    }
}
int main(void)
{
    unsigned char rom[0x1000]; MEM m={rom,sizeof(rom)}; NVX_FWSEC_INFO f;
    FILE *in; unsigned char *captured; long size;
    memset(rom,0,sizeof(rom));image(rom,0,1,0,0);image(rom,0x200,4,0xe0,0x80);
    bit_header(rom,0x80,0xa0,0x100);fwsec_v3(rom,0x100,0x140,0x140);
    assert(NvxParseFwsecRom(read32,&m,sizeof(rom),&f)==NVX_VBIOS_OK);
    assert(f.BiosSize==0xa00&&f.BitOffset==0x80&&f.FalconTableOffset==0x100);
    assert(f.DescriptorVersion==3&&f.DescriptorOffset==0x140&&f.ImageOffset==0x2ec);
    assert(f.StoredSize==0x500&&f.ImemLoadSize==0x100&&f.DmemLoadSize==0x400);
    assert(f.ApplicationId==0x85&&f.UcodeId==3&&f.SignatureCount==1);
    assert(f.SignaturesSize==384&&f.DmemImageOffset==0x3ec&&f.InterfaceImageOffset==0x3fc);
    assert(f.DmemMapperOffset==0x30&&f.CommandInOffset==0x100&&f.CommandOutOffset==0x140);
    {
        uint32_t version,index,offset;
        assert(NvxSelectFwsecSignature(&f,0,&version,&index,&offset));
        assert(version==0&&index==0&&offset==f.SignaturesOffset);
        f.SignatureVersions=7;f.SignatureCount=3;f.SignaturesSize=3*384;
        assert(NvxSelectFwsecSignature(&f,1,&version,&index,&offset));
        assert(version==1&&index==1&&offset==f.SignaturesOffset+384);
        assert(NvxSelectFwsecSignature(&f,2,&version,&index,&offset));
        assert(version==2&&index==2&&offset==f.SignaturesOffset+768);
        assert(!NvxSelectFwsecSignature(&f,4,&version,&index,&offset));
        f.SignatureVersions=1;f.SignatureCount=1;f.SignaturesSize=384;
    }
    {
        NVX_FRTS_LAYOUT layout;
        assert(NvxComputeFrtsLayout(0x200000000ULL,0,0,0,0,&layout));
        assert(layout.VgaWorkspaceOffset==0x1fff00000ULL);
        assert(layout.GspFwWprEnd==0x1fff00000ULL);
        assert(layout.FrtsOffset==0x1ffe00000ULL&&layout.FrtsSize4k==0x100);
        assert(NvxComputeFrtsLayout(0x200000000ULL,0,1,0x1ff0000,0x1ff1000,&layout));
        assert(layout.MmuLockValid&&layout.MmuLockLo==0x1ff000000ULL);
        assert(layout.GspFwWprEnd==0x1ff000000ULL&&layout.FrtsOffset==0x1fef00000ULL);
        assert(!NvxComputeFrtsLayout(0x1000,0,0,0,0,&layout));
    }
    p32(rom,0x144,0xfffffff0);assert(NvxParseFwsecRom(read32,&m,sizeof(rom),&f)==NVX_VBIOS_BAD_DESCRIPTOR);
    p32(rom,0x144,0x500);rom[0x8b]++;assert(NvxParseFwsecRom(read32,&m,sizeof(rom),&f)==NVX_VBIOS_NO_BIT);
    rom[0x8b]--;assert(NvxParseFwsecRom(read32,&m,0x200,&f)==NVX_VBIOS_INVALID_ROM);

    /* Reproduce the v35 live layout: a base image, a public EFI image, then
       an NVIDIA private VN/NPDS extension.  OpenRM biases extension-relative
       pointers by private-start minus base-size: 0x26800-0xfe00=0x16a00. */
    {
        const uint32_t n=0x8a800, expansion=0x16a00, table=0x88d83, desc=0x88e80;
        unsigned char *privateRom=calloc(1,n);assert(privateRom);
        m.p=privateRom;m.n=n;
        image(privateRom,0,0x7f,0,0);
        image(privateRom,0xfe00,0xb5,3,0);
        image_ex(privateRom,0x26800,0x320,0xe0,0x80,0x4e56,0x5344504e);
        bit_header(privateRom,0x1b0,0x300,0x72383);
        fwsec_v3(privateRom,table,desc,desc-expansion);
        assert(NvxParseFwsecRom(read32,&m,n,&f)==NVX_VBIOS_OK);
        assert(f.BiosSize==n&&f.ExpansionRomOffset==expansion);
        assert(f.BitOffset==0x1b0&&f.FalconTableOffset==table);
        assert(f.DescriptorOffset==desc&&f.ImageOffset==desc+0x1ac);
        assert(f.SignaturesSize==384&&f.DmemMapperOffset==0x30);
        privateRom[0x26800]=0x34;
        assert(NvxParseFwsecRom(read32,&m,n,&f)==NVX_VBIOS_INVALID_ROM);
        privateRom[0x26800]=0x56;privateRom[desc+0x1ac+0x100+0x10]=2;
        assert(NvxParseFwsecRom(read32,&m,n,&f)==NVX_VBIOS_BAD_INTERFACE);
        free(privateRom);
    }
    in=fopen("../firmware/ga104-vbios.rom","rb");
    if(in){fseek(in,0,SEEK_END);size=ftell(in);rewind(in);captured=malloc((size_t)size);assert(captured);
        assert(fread(captured,1,(size_t)size,in)==(size_t)size);fclose(in);m.p=captured;m.n=(size_t)size;
        assert(NvxParseFwsecRom(read32,&m,(uint32_t)size,&f)!=NVX_VBIOS_OK);free(captured);}
    puts("VBIOS tests passed: PCI/private ROM chain, BIT/Falcon/FWSEC v3, bounds, checksum, truncated capture.");
    return 0;
}
