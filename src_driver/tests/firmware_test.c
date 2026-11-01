#include "nvx_fw.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path,"rb");
    unsigned char *p;
    assert(f);
    assert(!fseek(f,0,SEEK_END));
    *size = (size_t)ftell(f);
    rewind(f);
    p = malloc(*size); assert(p);
    assert(fread(p,1,*size,f)==*size);
    fclose(f);
    return p;
}
static void put32(unsigned char *p, uint32_t v)
{
    unsigned i;
    for (i=0;i<4;i++) p[i]=(unsigned char)(v>>(i*8));
}
int main(void)
{
    size_t size, elfsize, i;
    NVX_BOOTER_INFO b;
    unsigned char *p=read_file("booter_load_535_113_01.bin",&size);
    unsigned char *elf=read_file("../dist_full/gsp_ga104.bin",&elfsize);
    unsigned char *copy=malloc(elfsize);
    uint32_t off,len;
    assert(copy);
    assert(NvxParseBooter(p,size,&b));
    assert(b.DataOffset==0x378 && b.DataSize==0xe600);
    assert(b.ImemOffset==0x100 && b.ImemSize==0x8300);
    assert(b.DmemOffset==0x8400 && b.DmemSize==0x6200);
    assert(b.PatchOffset==0x8410 && b.SignatureSize==384 && b.SignatureCount==2);
    assert(b.FuseVersion==1 && b.EngineId==1 && b.UcodeId==3);
    assert(NvxSelectSignature(0,1,2)==1); /* unfused GPU uses last signature */
    assert(NvxSelectSignature(1,1,2)==0);
    assert(NvxSelectSignature(3,1,2)==-1); /* revoked image */
    assert(NvxSelectSignature(1,10,2)==-1);
    assert(NvxSelectSignature(0,1,0)==-1);
    for(i=0;i<size;i++) assert(!NvxParseBooter(p,i,&b));
    put32(p+0x350,0); assert(!NvxParseBooter(p,size,&b)); put32(p+0x350,2);
    put32(p+0x33c,0xfffffff0); assert(!NvxParseBooter(p,size,&b)); put32(p+0x33c,0x8410);
    assert(NvxFindElfSection(elf,elfsize,".fwimage",&off,&len));
    assert(off==0x40 && len==0x2448000);
    assert(NvxFindElfSection(elf,elfsize,".fwsignature_ga10x",&off,&len));
    assert(off==0x244b04b && len==4096);
    assert(!NvxFindElfSection(elf,elfsize,".missing",&off,&len));
    assert(!NvxFindElfSection(elf,64,".fwimage",&off,&len));
    memcpy(copy,elf,elfsize); memset(copy+40,0xff,8);
    assert(!NvxFindElfSection(copy,elfsize,".fwimage",&off,&len));
    /* Malformed table offsets/sizes must remain bounded under sanitizers. */
    for(i=0;i<2000;i++) {
        size_t pos=(i*7919)%size;
        unsigned char saved=p[pos];
        p[pos]^=(unsigned char)(i|1);
        (void)NvxParseBooter(p,size,&b);
        p[pos]=saved;
    }
    memcpy(copy,elf,elfsize);
    for(i=0;i<2000;i++) {
        size_t pos=i%64;
        unsigned char saved=copy[pos];
        copy[pos]^=(unsigned char)(i|1);
        (void)NvxFindElfSection(copy,elfsize,".fwimage",&off,&len);
        copy[pos]=saved;
    }
    free(copy);free(p);free(elf);
    puts("Firmware tests passed: real containers, fuse selection, truncation, malformed inputs.");
    return 0;
}
