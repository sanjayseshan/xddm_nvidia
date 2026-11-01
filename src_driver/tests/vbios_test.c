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
static void image(unsigned char *p,uint32_t o,uint16_t blocks,unsigned char type,unsigned char last)
{
    p16(p,o,0xaa55);p16(p,o+0x18,0x20);p32(p,o+0x20,0x52494350);
    p16(p,o+0x2a,0x18);p16(p,o+0x30,blocks);p[o+0x34]=type;p[o+0x35]=last;
}
int main(void)
{
    unsigned char rom[0x1000]; MEM m={rom,sizeof(rom)}; NVX_FWSEC_INFO f;
    unsigned i,sum=0; FILE *in; unsigned char *captured; long size;
    memset(rom,0,sizeof(rom));image(rom,0,1,0,0);image(rom,0x200,2,0xe0,0x80);
    p16(rom,0x80,0xb8ff);p32(rom,0x82,0x00544942);p16(rom,0x86,0x0100);
    rom[0x88]=12;rom[0x89]=6;rom[0x8a]=1;
    for(i=0;i<11;i++) sum+=rom[0x80+i];
    rom[0x8b]=(unsigned char)(0-sum);
    rom[0x8c]=0x70;rom[0x8d]=2;p16(rom,0x8e,4);p16(rom,0x90,0xa0);p32(rom,0xa0,0x100);
    rom[0x100]=1;rom[0x101]=6;rom[0x102]=6;rom[0x103]=1;rom[0x104]=3;rom[0x105]=44;
    rom[0x106]=0x85;rom[0x107]=1;p32(rom,0x108,0x140);
    p32(rom,0x140,0x002c0301);p32(rom,0x144,0x100);p32(rom,0x148,0x20);
    p32(rom,0x14c,0x40);p32(rom,0x154,0x80);p32(rom,0x160,0x80);
    p16(rom,0x164,1);rom[0x166]=3;rom[0x167]=2;p16(rom,0x168,3);
    assert(NvxParseFwsecRom(read32,&m,sizeof(rom),&f)==NVX_VBIOS_OK);
    assert(f.BiosSize==0x600&&f.BitOffset==0x80&&f.FalconTableOffset==0x100);
    assert(f.DescriptorVersion==3&&f.DescriptorOffset==0x140&&f.ImageOffset==0x16c);
    assert(f.StoredSize==0x100&&f.ImemLoadSize==0x80&&f.DmemLoadSize==0x80);
    assert(f.ApplicationId==0x85&&f.UcodeId==3&&f.SignatureCount==2);
    p32(rom,0x144,0xfffffff0);assert(NvxParseFwsecRom(read32,&m,sizeof(rom),&f)==NVX_VBIOS_BAD_DESCRIPTOR);
    p32(rom,0x144,0x100);rom[0x8b]++;assert(NvxParseFwsecRom(read32,&m,sizeof(rom),&f)==NVX_VBIOS_NO_BIT);
    rom[0x8b]--;assert(NvxParseFwsecRom(read32,&m,0x200,&f)==NVX_VBIOS_INVALID_ROM);
    in=fopen("../firmware/ga104-vbios.rom","rb");
    if(in){fseek(in,0,SEEK_END);size=ftell(in);rewind(in);captured=malloc((size_t)size);assert(captured);
        assert(fread(captured,1,(size_t)size,in)==(size_t)size);fclose(in);m.p=captured;m.n=(size_t)size;
        assert(NvxParseFwsecRom(read32,&m,(uint32_t)size,&f)!=NVX_VBIOS_OK);free(captured);}
    puts("VBIOS tests passed: ROM chain, BIT/Falcon/FWSEC v3, bounds, checksum, truncated capture.");
    return 0;
}
