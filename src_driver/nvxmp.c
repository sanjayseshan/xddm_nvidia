/*
 * NVIDIA GeForce RTX 3070 (Ampere GA104) XDDM Video Miniport Driver
 * Target: Windows XP Professional x64 Edition / Server 2003 x64 (NT 5.2)
 *
 * Driver Version: 1.0.0.136
 */

#include "miniport.h"
#include "nvx_gsp.h"
#include "nvx_vbe.h"
#include "nvx_vbios.h"

#define NV_VENDOR_ID 0x10DE

/* COM1 Direct Serial Logging (I/O Port 0x3F8) */
static inline unsigned char inb(unsigned short port) {
    unsigned char val;
    __asm__ __volatile__("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outb(unsigned short port, unsigned char val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

void LogStr(const char *s) {
    while (*s) {
        if (*s == '\n') {
            while ((inb(0x3FD) & 0x20) == 0);
            outb(0x3F8, '\r');
        }
        while ((inb(0x3FD) & 0x20) == 0);
        outb(0x3F8, (unsigned char)*s++);
    }
}

void LogHex(unsigned long long val) {
    char buf[19];
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 15; i >= 0; i--) {
        int nibble = val & 0xF;
        buf[2 + i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
        val >>= 4;
    }
    buf[18] = '\0';
    LogStr(buf);
}

typedef struct _NVX_PROM_CONTEXT { PUCHAR Bar0; } NVX_PROM_CONTEXT;

static int NvxPromRead32(void *context, uint32_t offset, uint32_t *value)
{
    NVX_PROM_CONTEXT *prom=(NVX_PROM_CONTEXT *)context;
    if (!prom || !prom->Bar0 || !value || offset > 0xffffc || (offset & 3)) return 0;
    *value=VideoPortReadRegisterUlong((PULONG)(prom->Bar0+0x300000+offset));
    return 1;
}

static int NvxPromCopy(NVX_PROM_CONTEXT *prom, uint32_t offset, PUCHAR dst, uint32_t size)
{
    uint32_t i,value=0,aligned=0xffffffffU;
    for (i=0;i<size;i++) {
        uint32_t current=(offset+i)&~3U;
        if (current!=aligned) {
            if (!NvxPromRead32(prom,current,&value)) return 0;
            aligned=current;
        }
        dst[i]=(UCHAR)(value>>(8*((offset+i)&3U)));
    }
    return 1;
}

static uint32_t NvxCrc32(const UCHAR *data, uint32_t size)
{
    uint32_t crc=0xffffffffU,i;
    for (i=0;i<size;i++) {
        unsigned bit;crc^=data[i];
        for (bit=0;bit<8;bit++) crc=(crc>>1)^((0U-(crc&1U))&0xedb88320U);
    }
    return crc^0xffffffffU;
}

static VOID NvxPut32(PUCHAR p,uint32_t value)
{
    p[0]=(UCHAR)value;p[1]=(UCHAR)(value>>8);p[2]=(UCHAR)(value>>16);p[3]=(UCHAR)(value>>24);
}

static VOID NvxLogFwsecExecutionPlan(PUCHAR bar0,const NVX_FWSEC_INFO *info,
                                     PHYSICAL_ADDRESS physical)
{
    if (!bar0 || !info || physical.QuadPart<0 ||
        (uint64_t)physical.QuadPart<info->ImemVirtBase ||
        (info->ImemLoadSize&0xffU) || (info->DmemLoadSize&0xffU)) {
        LogStr("NVX [FWSEC-PLAN]: invalid DMA alignment/address inputs; execution blocked.\n");
        return;
    }
    uint64_t imemSource=(uint64_t)physical.QuadPart-info->ImemVirtBase;
    uint64_t dmemSource=(uint64_t)physical.QuadPart+info->ImemLoadSize;
    LogStr("NVX [FWSEC-PLAN]: imem_src=");LogHex(imemSource);
    LogStr(" base256=");LogHex(imemSource>>8);
    LogStr(" mem_off=");LogHex(info->ImemVirtBase);
    LogStr(" dst=");LogHex(info->ImemPhysBase);
    LogStr(" bytes=");LogHex(info->ImemLoadSize);
    LogStr(" blocks256=");LogHex(info->ImemLoadSize>>8);
    LogStr(" cmd=0x614\n");
    LogStr("NVX [FWSEC-PLAN]: dmem_src=");LogHex(dmemSource);
    LogStr(" base256=");LogHex(dmemSource>>8);
    LogStr(" mem_off=0x0 dst=");LogHex(info->DmemPhysBase);
    LogStr(" bytes=");LogHex(info->DmemLoadSize);
    LogStr(" blocks256=");LogHex(info->DmemLoadSize>>8);
    LogStr(" cmd=0x600\n");
    LogStr("NVX [FWSEC-PLAN]: brom paraaddr=");LogHex(info->PkcDataOffset);
    LogStr(" engidmask=");LogHex(info->EngineIdMask);
    LogStr(" ucode_id=");LogHex(info->UcodeId);
    LogStr(" mod_sel=0x1 bootvec=");LogHex(info->ImemVirtBase);LogStr("\n");
    LogStr("NVX [FWSEC-PLAN]: baseline engine=");
    LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x1103c0)));
    LogStr(" hwcfg2=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x1100f4)));
    LogStr(" dmactl=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x11010c)));
    LogStr(" cpuctl=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x110100)));
    LogStr(" fbif_ctl=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x110624)));
    LogStr(" transcfg0=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x110600)));
    LogStr("\nNVX [FWSEC-PLAN]: riscv_cpuctl=");
    LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x111388)));
    LogStr(" bcr=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x111668)));
    LogStr(" plan reset=1->0 switch_bcr=0 wait_valid=1 transcfg0_target=0x5\n");
    LogStr("NVX [FWSEC-PLAN]: read-only execution plan complete; no GPU writes.\n");
}

static BOOLEAN NvxWaitMask(PUCHAR bar0,ULONG offset,ULONG mask,ULONG expected,
                           ULONG iterations,ULONG stallUs)
{
    while (iterations--) {
        if ((VideoPortReadRegisterUlong((PULONG)(bar0+offset))&mask)==expected)
            return TRUE;
        VideoPortStallExecution(stallUs);
    }
    return FALSE;
}

static BOOLEAN NvxFwsecDma(PUCHAR bar0,uint64_t source,uint32_t destination,
                            uint32_t memoryOffset,uint32_t size,uint32_t command,
                            BOOLEAN *retainAllocation)
{
    uint32_t offset;
    if (!NvxWaitMask(bar0,0x110118,1,0,20000,5)) {
        *retainAllocation=TRUE;
        LogStr("NVX [FWSEC-EXEC]: DMA queue full before base programming; allocation retained.\n");
        return FALSE;
    }
    VideoPortWriteRegisterUlong((PULONG)(bar0+0x110110),(ULONG)(source>>8));
    VideoPortWriteRegisterUlong((PULONG)(bar0+0x110128),(ULONG)((source>>40)&0x1ff));
    for (offset=0;offset<size;offset+=0x100) {
        if (!NvxWaitMask(bar0,0x110118,1,0,20000,5)) {
            *retainAllocation=TRUE;
            LogStr("NVX [FWSEC-EXEC]: DMA request queue stayed full at offset=");
            LogHex(offset);LogStr(" dmacmd=");
            LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x110118)));
            LogStr(" dmactl=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x11010c)));
            LogStr("; allocation retained.\n");
            return FALSE;
        }
        VideoPortWriteRegisterUlong((PULONG)(bar0+0x110114),destination+offset);
        VideoPortWriteRegisterUlong((PULONG)(bar0+0x11011c),memoryOffset+offset);
        VideoPortWriteRegisterUlong((PULONG)(bar0+0x110118),command);
    }
    if (!NvxWaitMask(bar0,0x110118,2,2,20000,5)) {
        *retainAllocation=TRUE;
        LogStr("NVX [FWSEC-EXEC]: DMA engine-idle timeout; allocation retained.\n");
        return FALSE;
    }
    return TRUE;
}

static VOID NvxLogFwsecExecState(PUCHAR bar0,const char *where)
{
    LogStr("NVX [FWSEC-STATE]: ");LogStr(where);
    LogStr(" engine=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x1103c0)));
    LogStr(" hwcfg2=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x1100f4)));
    LogStr(" cpuctl=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x110100)));
    LogStr(" bootvec=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x110104)));
    LogStr(" dmactl=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x11010c)));
    LogStr(" dmacmd=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x110118)));
    LogStr(" irqstat=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x110008)));
    LogStr("\nNVX [FWSEC-STATE]: mailbox0=");
    LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x110040)));
    LogStr(" mailbox1=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x110044)));
    LogStr(" riscv_cpuctl=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x111388)));
    LogStr(" bcr=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x111668)));
    LogStr(" mod_sel=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x111180)));
    LogStr(" ucode_id=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x111198)));
    LogStr(" engidmask=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x11119c)));
    LogStr(" paraaddr=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x111210)));
    LogStr("\nNVX [FWSEC-STATE]: scratch0e=");
    LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x1438)));
    LogStr(" wpr2_lo=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x1fa824)));
    LogStr(" wpr2_hi=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x1fa828)));
    LogStr("\n");
}

static BOOLEAN NvxExecuteFwsecFrts(PVOID hwDeviceExtension,PUCHAR bar0,
                                    const NVX_FWSEC_INFO *info,
                                    const NVX_FRTS_LAYOUT *layout,
                                    PHYSICAL_ADDRESS physical,
                                    BOOLEAN *retainAllocation)
{
    ULONG value,originalEngine,cpuCtl,lastCtl,iterations;
    BOOLEAN sawRunning=FALSE,confirmedHalt=FALSE;
    uint64_t imemSource=(uint64_t)physical.QuadPart-info->ImemVirtBase;
    uint64_t dmemSource=(uint64_t)physical.QuadPart+info->ImemLoadSize;
    PNVX_DEVICE_EXTENSION devExt=(PNVX_DEVICE_EXTENSION)hwDeviceExtension;
    *retainAllocation=FALSE;
    LogStr("NVX [FWSEC-EXEC]: beginning guarded GSP-Falcon FRTS execution.\n");

    /* OpenRM treats RESET_READY as advisory on GA10x (bug 3419321). */
    for (iterations=0;iterations<150;iterations++) {
        if (VideoPortReadRegisterUlong((PULONG)(bar0+0x1100f4))&0x80000000U) break;
        VideoPortStallExecution(1);
    }
    originalEngine=VideoPortReadRegisterUlong((PULONG)(bar0+0x1103c0));
    VideoPortWriteRegisterUlong((PULONG)(bar0+0x1103c0),originalEngine|1U);
    for (iterations=0;iterations<10;iterations++)
        (void)VideoPortReadRegisterUlong((PULONG)(bar0+0x1103c0));
    VideoPortWriteRegisterUlong((PULONG)(bar0+0x1103c0),originalEngine&~1U);
    for (iterations=0;iterations<10;iterations++)
        (void)VideoPortReadRegisterUlong((PULONG)(bar0+0x1103c0));
    if (!NvxWaitMask(bar0,0x1100f4,0x1000,0,2000,10)) {
        LogStr("NVX [FWSEC-EXEC]: reset scrub timeout before DMA.\n");return FALSE;
    }
    value=VideoPortReadRegisterUlong((PULONG)(bar0+0x1100f4));
    if (!(value&0x400)) {
        LogStr("NVX [FWSEC-EXEC]: GSP RISC-V/Falcon complex disabled.\n");return FALSE;
    }
    value=VideoPortReadRegisterUlong((PULONG)(bar0+0x111668));
    if (value&0x10) {
        VideoPortWriteRegisterUlong((PULONG)(bar0+0x111668),0);
        if (!NvxWaitMask(bar0,0x111668,1,1,20000,5)) {
            LogStr("NVX [FWSEC-EXEC]: Falcon core-select timeout before DMA.\n");return FALSE;
        }
    }
    VideoPortWriteRegisterUlong((PULONG)(bar0+0x110084),
        VideoPortReadRegisterUlong((PULONG)bar0));
    value=VideoPortReadRegisterUlong((PULONG)(bar0+0x110624));
    VideoPortWriteRegisterUlong((PULONG)(bar0+0x110624),value|0x80U);
    /* OpenRM also clears DMACTL to permit requests without a block bind. */
    VideoPortWriteRegisterUlong((PULONG)(bar0+0x11010c),0);
    value=VideoPortReadRegisterUlong((PULONG)(bar0+0x110600));
    VideoPortWriteRegisterUlong((PULONG)(bar0+0x110600),(value&~7U)|5U);

    if (!NvxFwsecDma(bar0,imemSource,info->ImemPhysBase,info->ImemVirtBase,
                     info->ImemLoadSize,0x614,retainAllocation) ||
        !NvxFwsecDma(bar0,dmemSource,info->DmemPhysBase,0,
                     info->DmemLoadSize,0x600,retainAllocation)) return FALSE;

    VideoPortWriteRegisterUlong((PULONG)(bar0+0x111210),info->PkcDataOffset);
    VideoPortWriteRegisterUlong((PULONG)(bar0+0x11119c),info->EngineIdMask);
    VideoPortWriteRegisterUlong((PULONG)(bar0+0x111198),info->UcodeId);
    VideoPortWriteRegisterUlong((PULONG)(bar0+0x111180),1);
    VideoPortWriteRegisterUlong((PULONG)(bar0+0x110104),info->ImemVirtBase);
    NvxLogFwsecExecState(bar0,"before start");
    cpuCtl=VideoPortReadRegisterUlong((PULONG)(bar0+0x110100));
    if (cpuCtl&0x40) VideoPortWriteRegisterUlong((PULONG)(bar0+0x110130),2);
    else VideoPortWriteRegisterUlong((PULONG)(bar0+0x110100),2);

    lastCtl=cpuCtl;
    for (iterations=0;iterations<40000;iterations++) {
        ULONG ctl=VideoPortReadRegisterUlong((PULONG)(bar0+0x110100));
        if (ctl!=lastCtl) {
            LogStr("NVX [FWSEC-EXEC]: CPUCTL transition ");LogHex(lastCtl);
            LogStr(" -> ");LogHex(ctl);LogStr("\n");lastCtl=ctl;
        }
        if (!(ctl&0x10)) sawRunning=TRUE;
        if (ctl&0x10) {
            ULONG wprLo=VideoPortReadRegisterUlong((PULONG)(bar0+0x1fa824));
            ULONG wprHi=VideoPortReadRegisterUlong((PULONG)(bar0+0x1fa828));
            ULONG scratch=VideoPortReadRegisterUlong((PULONG)(bar0+0x1438));
            if (sawRunning || NvxValidateFrtsWpr2(layout->FrtsOffset,scratch,wprLo,wprHi)) {
                confirmedHalt=TRUE;break;
            }
        }
        VideoPortStallExecution(50);
    }
    if (!confirmedHalt) {
        *retainAllocation=TRUE;
        NvxLogFwsecExecState(bar0,"unconfirmed completion");
        LogStr("NVX [FWSEC-EXEC]: no confirmed run-to-halt; allocation retained.\n");
        return FALSE;
    }
    {
        ULONG scratch=VideoPortReadRegisterUlong((PULONG)(bar0+0x1438));
        ULONG wprLo=VideoPortReadRegisterUlong((PULONG)(bar0+0x1fa824));
        ULONG wprHi=VideoPortReadRegisterUlong((PULONG)(bar0+0x1fa828));
        LogStr("NVX [FWSEC-EXEC]: halted cpuctl=");LogHex(lastCtl);
        LogStr(" mailbox0=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x110040)));
        LogStr(" mailbox1=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x110044)));
        LogStr(" scratch0e=");LogHex(scratch);
        LogStr(" wpr2_lo=");LogHex(wprLo);LogStr(" wpr2_hi=");LogHex(wprHi);LogStr("\n");
        if (!NvxValidateFrtsWpr2(layout->FrtsOffset,scratch,wprLo,wprHi)) {
            LogStr("NVX [FWSEC-EXEC]: FRTS/WPR2 validation failed.\n");return FALSE;
        }
    }
    devExt->FwsecFrtsReady=TRUE;
    devExt->VerifiedFrtsOffset=layout->FrtsOffset;
    devExt->VerifiedWprEnd=layout->GspFwWprEnd;
    devExt->VerifiedVgaWorkspaceOffset=layout->VgaWorkspaceOffset;
    LogStr("NVX [FWSEC-EXEC]: FRTS initialized and WPR2 validated.\n");
    return TRUE;
}

static VOID NvxFwsecDmaStageAndExecute(PVOID hwDeviceExtension,NVX_PROM_CONTEXT *prom,
                                   const NVX_FWSEC_INFO *info,
                                   const NVX_FRTS_LAYOUT *layout,
                                   uint32_t signatureOffset)
{
    uint32_t sigDst,mapper,cmd,baseCrc,stagedCrc,sigCrc;
    ULONG dmaLength;
    VP_DEVICE_DESCRIPTION devDesc;
    PVOID dmaAdapter=NULL;
    PUCHAR image=NULL;
    PHYSICAL_ADDRESS physical;
    BOOLEAN retainAllocation=FALSE;
    PNVX_DEVICE_EXTENSION devExt=(PNVX_DEVICE_EXTENSION)hwDeviceExtension;
    physical.QuadPart=0;
    if (!hwDeviceExtension||!prom||!info||!layout||!info->StoredSize||
        info->StoredSize>0x10000U||info->ImemLoadSize>info->StoredSize||
        info->PkcDataOffset>info->DmemLoadSize) {
        LogStr("NVX [FWSEC-DMA]: invalid image bounds; execution blocked.\n");return;
    }
    dmaLength=(info->StoredSize+0xfffU)&~0xfffU;
    VideoPortZeroMemory(&devDesc,sizeof(devDesc));
    devDesc.ScatterGather=TRUE;
    devDesc.Dma32BitAddresses=TRUE;
    devDesc.Dma64BitAddresses=FALSE;
    devDesc.MaximumLength=dmaLength;
    dmaAdapter=VideoPortGetDmaAdapter(hwDeviceExtension,&devDesc);
    LogStr("NVX [FWSEC-DMA]: adapter=");LogHex((ULONG64)dmaAdapter);
    LogStr(" requested=");LogHex(dmaLength);LogStr("\n");
    if (!dmaAdapter) {
        LogStr("NVX [FWSEC-DMA]: adapter unavailable; execution blocked.\n");return;
    }
    image=(PUCHAR)VideoPortAllocateCommonBuffer(hwDeviceExtension,dmaAdapter,dmaLength,
                                                &physical,TRUE,NULL);
    if (!image) {
        LogStr("NVX [FWSEC-DMA]: common-buffer allocation failed; execution blocked.\n");
        VideoPortPutDmaAdapter(hwDeviceExtension,dmaAdapter);return;
    }
    LogStr("NVX [FWSEC-DMA]: virtual=");LogHex((ULONG64)image);
    LogStr(" physical=");LogHex((ULONG64)physical.QuadPart);
    LogStr(" length=");LogHex(dmaLength);LogStr("\n");
    if (physical.QuadPart<0 || ((ULONG64)physical.QuadPart&0xfffULL) ||
        (ULONG64)physical.QuadPart>0x100000000ULL-dmaLength) {
        LogStr("NVX [FWSEC-DMA]: buffer is not page-aligned/below 4GB; execution blocked.\n");
        goto cleanup;
    }
    VideoPortZeroMemory(image,dmaLength);
    if (!NvxPromCopy(prom,info->ImageOffset,image,info->StoredSize)) {
        LogStr("NVX [FWSEC-DMA]: PROM copy failed; execution blocked.\n");goto cleanup;
    }
    baseCrc=NvxCrc32(image,info->StoredSize);
    if (baseCrc!=info->ImageCrc32) {
        LogStr("NVX [FWSEC-DMA]: PROM image CRC changed; execution blocked.\n");
        goto cleanup;
    }
    sigDst=info->ImemLoadSize+info->PkcDataOffset;
    mapper=info->ImemLoadSize+info->DmemMapperOffset;
    cmd=info->ImemLoadSize+info->CommandInOffset;
    if (sigDst>info->StoredSize || 384U>info->StoredSize-sigDst ||
        mapper>info->StoredSize || 64U>info->StoredSize-mapper ||
        cmd>info->StoredSize || 44U>info->StoredSize-cmd ||
        !NvxPromCopy(prom,signatureOffset,image+sigDst,384U)) {
        LogStr("NVX [FWSEC-DMA]: patch bounds/read invalid; execution blocked.\n");
        goto cleanup;
    }
    sigCrc=NvxCrc32(image+sigDst,384U);
    NvxPut32(image+mapper+0x2c,0x15U);
    VideoPortZeroMemory(image+cmd,44U);
    NvxPut32(image+cmd+0,1U);NvxPut32(image+cmd+4,24U);
    NvxPut32(image+cmd+20,2U);
    NvxPut32(image+cmd+24,1U);NvxPut32(image+cmd+28,20U);
    NvxPut32(image+cmd+32,(uint32_t)(layout->FrtsOffset>>12));
    NvxPut32(image+cmd+36,layout->FrtsSize4k);NvxPut32(image+cmd+40,2U);
    stagedCrc=NvxCrc32(image,info->StoredSize);
    LogStr("NVX [FWSEC-DMA]: base_crc=");LogHex(baseCrc);
    LogStr(" expected=");LogHex(info->ImageCrc32);
    LogStr(" selected_sig_crc=");LogHex(sigCrc);
    LogStr(" staged_crc=");LogHex(stagedCrc);LogStr("\n");
    LogStr("NVX [FWSEC-DMA]: sig_dmem=");LogHex(info->PkcDataOffset);
    LogStr(" mapper_dmem=");LogHex(info->DmemMapperOffset);
    LogStr(" init_cmd=0x15 cmd_dmem=");LogHex(info->CommandInOffset);
    LogStr(" bytes=0x2C coherent DMA staging complete; guarded execution follows.\n");
    NvxLogFwsecExecutionPlan(prom->Bar0,info,physical);
    if (!NvxExecuteFwsecFrts(hwDeviceExtension,prom->Bar0,info,layout,physical,
                             &retainAllocation))
        LogStr("NVX [FWSEC-EXEC]: execution did not validate; GSP boot remains blocked.\n");
cleanup:
    if (retainAllocation) {
        devExt->FwsecDmaAdapter=dmaAdapter;
        devExt->FwsecDmaPhysical=physical;
        devExt->FwsecDmaVirtual=image;
        devExt->FwsecDmaLength=dmaLength;
        LogStr("NVX [FWSEC-DMA]: retained for hardware safety; not scrubbed or freed.\n");
        return;
    }
    VideoPortZeroMemory(image,dmaLength);
    VideoPortFreeCommonBuffer(hwDeviceExtension,dmaLength,image,physical,TRUE);
    VideoPortPutDmaAdapter(hwDeviceExtension,dmaAdapter);
    LogStr("NVX [FWSEC-DMA]: buffer scrubbed and released.\n");
}

static VOID NvxProbeFwsecRom(PVOID hwDeviceExtension,PUCHAR bar0)
{
    NVX_PROM_CONTEXT prom;
    NVX_FWSEC_INFO info;
    NVX_FRTS_LAYOUT frtsLayout;
    uint32_t selectedSignatureOffset=0;
    int signatureSelected=0,frtsLayoutValid=0;
    int status;
    prom.Bar0=bar0;
    {
        static const uint32_t offsets[] = { 0, 0x18, 0x170, 0xfe00, 0x26800, 0x88d80 };
        unsigned i;
        LogStr("NVX [VBIOS]: Raw PROM dwords");
        for (i=0; i<sizeof(offsets)/sizeof(offsets[0]); i++) {
            uint32_t value=0;
            LogStr(" [");LogHex(offsets[i]);LogStr("]=");
            if (NvxPromRead32(&prom,offsets[i],&value)) LogHex(value);
            else LogStr("READ_ERROR");
        }
        LogStr("\n");
    }
    status=NvxParseFwsecRom(NvxPromRead32,&prom,0x100000,&info);
    LogStr("NVX [VBIOS]: Read-only FWSEC probe status ");LogHex((ULONG)status);LogStr("\n");
    if (info.DescriptorVersion==3) {
        LogStr("NVX [FWSEC]: validation_stage=");LogHex(info.ValidationStage);
        LogStr(" desc=");LogHex(info.DescriptorOffset);
        LogStr(" size=");LogHex(info.DescriptorSize);
        LogStr(" image=");LogHex(info.ImageOffset);LogStr("\n");
        LogStr("NVX [FWSEC]: partial iface_image=");LogHex(info.InterfaceImageOffset);
        LogStr(" iface_v=");LogHex(info.InterfaceVersion);
        LogStr(" header=");LogHex(info.InterfaceHeaderSize);
        LogStr(" entry_size=");LogHex(info.InterfaceEntrySize);
        LogStr(" entries=");LogHex(info.InterfaceEntryCount);LogStr("\n");
        LogStr("NVX [FWSEC]: partial mapper_dmem=");LogHex(info.DmemMapperOffset);
        LogStr(" signature=");LogHex(info.DmemMapperSignature);
        LogStr(" version=");LogHex(info.DmemMapperVersion);
        LogStr(" size=");LogHex(info.DmemMapperSize);LogStr("\n");
        LogStr("NVX [FWSEC]: partial cmd_in=");LogHex(info.CommandInOffset);
        LogStr(" size=");LogHex(info.CommandInSize);
        LogStr(" cmd_out=");LogHex(info.CommandOutOffset);
        LogStr(" size=");LogHex(info.CommandOutSize);
        LogStr(" valid=");LogHex(info.CommandOutValid);LogStr("\n");
    }
    if (status==NVX_VBIOS_OK) {
        LogStr("NVX [VBIOS]: size=");LogHex(info.BiosSize);
        LogStr(" expansion=");LogHex(info.ExpansionRomOffset);
        LogStr(" BIT=");LogHex(info.BitOffset);
        LogStr(" FalconTable=");LogHex(info.FalconTableOffset);LogStr("\n");
        LogStr("NVX [FWSEC]: desc=");LogHex(info.DescriptorOffset);
        LogStr(" version=");LogHex(info.DescriptorVersion);
        LogStr(" size=");LogHex(info.DescriptorSize);
        LogStr(" image=");LogHex(info.ImageOffset);
        LogStr(" stored=");LogHex(info.StoredSize);LogStr("\n");
        LogStr("NVX [FWSEC]: imem=");LogHex(info.ImemLoadSize);
        LogStr(" dmem=");LogHex(info.DmemLoadSize);
        LogStr(" interface=");LogHex(info.InterfaceOffset);
        LogStr(" ucode=");LogHex(info.UcodeId);
        LogStr(" signatures=");LogHex(info.SignatureCount);LogStr("\n");
        LogStr("NVX [FWSEC]: pkc_dmem=");LogHex(info.PkcDataOffset);
        LogStr(" sig_bytes=");LogHex(info.SignaturesSize);
        LogStr(" sig_versions=");LogHex(info.SignatureVersions);
        LogStr(" sig_crc32=");LogHex(info.SignaturesCrc32);
        LogStr(" image_crc32=");LogHex(info.ImageCrc32);LogStr("\n");
        LogStr("NVX [FWSEC]: interface_image=");LogHex(info.InterfaceImageOffset);
        LogStr(" iface_v=");LogHex(info.InterfaceVersion);
        LogStr(" entries=");LogHex(info.InterfaceEntryCount);
        LogStr(" mapper_dmem=");LogHex(info.DmemMapperOffset);
        LogStr(" mapper_v=");LogHex(info.DmemMapperVersion);LogStr("\n");
        LogStr("NVX [FWSEC]: cmd_in=");LogHex(info.CommandInOffset);
        LogStr(" size=");LogHex(info.CommandInSize);
        LogStr(" cmd_out=");LogHex(info.CommandOutOffset);
        LogStr(" size=");LogHex(info.CommandOutSize);
        LogStr(" valid=");LogHex(info.CommandOutValid);LogStr("\n");
        if (info.UcodeId>=1 && info.UcodeId<=16) {
            uint32_t fuseOffset=0x8241c0U+4U*((uint32_t)info.UcodeId-1U);
            uint32_t rawFuse=VideoPortReadRegisterUlong((PULONG)(bar0+fuseOffset));
            uint32_t fuseVersion=0,signatureIndex=0,signatureOffset=0;
            LogStr("NVX [FWSEC]: GSP fuse offset=");LogHex(fuseOffset);
            LogStr(" raw=");LogHex(rawFuse);
            if (NvxSelectFwsecSignature(&info,rawFuse,&fuseVersion,
                                        &signatureIndex,&signatureOffset)) {
                LogStr(" version=");LogHex(fuseVersion);
                LogStr(" signature_index=");LogHex(signatureIndex);
                LogStr(" signature_rom=");LogHex(signatureOffset);LogStr("\n");
                selectedSignatureOffset=signatureOffset;signatureSelected=1;
            } else {
                LogStr(" no compatible VBIOS signature\n");
            }
        }
        {
            uint64_t fbSize=(uint64_t)VideoPortReadRegisterUlong((PULONG)(bar0+0x1183a4))<<20;
            uint32_t workspace=VideoPortReadRegisterUlong((PULONG)(bar0+0x625f04));
            uint32_t lockPlm=VideoPortReadRegisterUlong((PULONG)(bar0+0x1fa7c8));
            uint32_t lockLo=VideoPortReadRegisterUlong((PULONG)(bar0+0x1fa82c));
            uint32_t lockHi=VideoPortReadRegisterUlong((PULONG)(bar0+0x1fa830));
            uint32_t wpr2Lo=VideoPortReadRegisterUlong((PULONG)(bar0+0x1fa824));
            uint32_t wpr2Hi=VideoPortReadRegisterUlong((PULONG)(bar0+0x1fa828));
            LogStr("NVX [FRTS]: regs fb=");LogHex(fbSize);
            LogStr(" workspace=");LogHex(workspace);
            LogStr(" lock_plm=");LogHex(lockPlm);
            LogStr(" lock_lo=");LogHex(lockLo);
            LogStr(" lock_hi=");LogHex(lockHi);
            LogStr(" wpr2_lo=");LogHex(wpr2Lo);
            LogStr(" wpr2_hi=");LogHex(wpr2Hi);LogStr("\n");
            if (NvxComputeFrtsLayout(fbSize,workspace,lockPlm,lockLo,lockHi,&frtsLayout)) {
                frtsLayoutValid=1;
                LogStr("NVX [FRTS]: vga=");LogHex(frtsLayout.VgaWorkspaceOffset);
                LogStr(" reserved=");LogHex(frtsLayout.VbiosReservedOffset);
                LogStr(" wpr_end=");LogHex(frtsLayout.GspFwWprEnd);
                LogStr(" frts=");LogHex(frtsLayout.FrtsOffset);
                LogStr(" size4k=");LogHex(frtsLayout.FrtsSize4k);LogStr("\n");
                LogStr("NVX [FRTS]: dry-run cmd=0x15 bytes=0x2C input_capacity=");
                LogHex(info.CommandInSize);
                LogStr(" region_offset4k=");LogHex(frtsLayout.FrtsOffset>>12);
                LogStr(" region_size4k=0x100 media=0x2\n");
            } else LogStr("NVX [FRTS]: layout invalid; execution remains blocked.\n");
        }
        if (signatureSelected&&frtsLayoutValid)
            NvxFwsecDmaStageAndExecute(hwDeviceExtension,&prom,&info,&frtsLayout,
                                      selectedSignatureOffset);
    }
}

typedef struct _GPU_DEVICE_ID {
    USHORT DeviceId;
    PCWSTR Name;
} GPU_DEVICE_ID;

static const GPU_DEVICE_ID SupportedGpus[] __attribute__((unused)) = {
    { 0x2484, L"NVIDIA GeForce RTX 3070 (Desktop)" },
    { 0x2488, L"NVIDIA GeForce RTX 3070 (Desktop GA104-LHR)" },
    { 0x249D, L"NVIDIA GeForce RTX 3070 (Mobile GA104)" },
    { 0x24A0, L"NVIDIA GeForce RTX 3070 Ti (Mobile)" },
    { 0x2486, L"NVIDIA GeForce RTX 3060 Ti" },
    { 0x2489, L"NVIDIA GeForce RTX 3060 Ti GA104" },
    { 0x2487, L"NVIDIA GeForce RTX 3060" },
    { 0x24C7, L"NVIDIA GeForce RTX 3060 GA104" },
    { 0, NULL }
};

BOOLEAN HwVidInterrupt(PVOID HwDeviceExtension) {
    (void)HwDeviceExtension;
    /* RM queue interrupts need their own dispatcher/acknowledgement path. */
    return FALSE;
}

BOOLEAN HwVidResetHw(PVOID HwDeviceExtension, ULONG Columns, ULONG Rows) {
    (void)HwDeviceExtension;
    (void)Columns;
    (void)Rows;
    /* VideoPort can call this at bugcheck/high IRQL.  Do not stall, reset a
       Falcon, release common buffers, or destroy a DMA adapter here. */
    LogStr("NVX [RESET]: HwVidResetHw invoked; allocations left intact.\n");
    return TRUE;
}

VP_STATUS HwVidSetPowerState(
    PVOID HwDeviceExtension,
    ULONG HwId,
    PVIDEO_POWER_MANAGEMENT VideoPowerControl
) {
    (void)HwDeviceExtension;
    (void)HwId;
    (void)VideoPowerControl;
    return NO_ERROR;
}

VP_STATUS HwVidGetPowerState(
    PVOID HwDeviceExtension,
    ULONG HwId,
    PVIDEO_POWER_MANAGEMENT VideoPowerControl
) {
    (void)HwDeviceExtension;
    (void)HwId;
    if (VideoPowerControl) {
        VideoPowerControl->PowerState = VideoPowerOn;
    }
    return NO_ERROR;
}

VP_STATUS HwVidGetVideoChildDescriptor(
    PVOID HwDeviceExtension,
    PVIDEO_CHILD_ENUM_INFO ChildEnumInfo,
    PVIDEO_CHILD_TYPE VideoChildType,
    PUCHAR pChildDescriptor,
    PULONG UId,
    PULONG pUnused
) {
    PNVX_DEVICE_EXTENSION dev=(PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    ULONG index;
    if(pUnused) *pUnused=0;
    if(!ChildEnumInfo || !VideoChildType || !UId)
        return VIDEO_ENUM_INVALID_DEVICE;
    LogStr("NVX: HwVidGetVideoChildDescriptor ChildIndex: ");
    LogHex(ChildEnumInfo->ChildIndex);LogStr("\n");
    if(ChildEnumInfo->ChildIndex==0xffffffffUL) {
        *VideoChildType=VideoChip;*UId=0xffffffffUL;
        return VIDEO_ENUM_MORE_DEVICES;
    }
    if(!ChildEnumInfo->ChildIndex) {
        for(index=0;index<dev->MonitorCount;index++)
            if(dev->MonitorIds[index]==ChildEnumInfo->ACPIHwId) break;
        if(index==dev->MonitorCount) return VIDEO_ENUM_INVALID_DEVICE;
    } else {
        index=ChildEnumInfo->ChildIndex-1;
        if(index>=dev->MonitorCount) return VIDEO_ENUM_NO_MORE_DEVICES;
    }
    if(!pChildDescriptor || ChildEnumInfo->ChildDescriptorSize<128)
        return VIDEO_ENUM_INVALID_DEVICE;
    /* XP's monitor child descriptor is the validated base EDID block. */
    VideoPortMoveMemory(pChildDescriptor,dev->MonitorEdid[index],128);
    *VideoChildType=Monitor;*UId=dev->MonitorIds[index];
    LogStr("NVX [DISPLAY]: enumerate monitor id=");LogHex(*UId);LogStr("\n");
    return VIDEO_ENUM_MORE_DEVICES;
}

VP_STATUS HwVidFindAdapter(
    PVOID HwDeviceExtension,
    PVOID HwContext,
    PWSTR ArgumentString,
    PVIDEO_PORT_CONFIG_INFO ConfigInfo,
    PUCHAR Again
) {
    PNVX_DEVICE_EXTENSION devExt = (PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    VIDEO_ACCESS_RANGE accessRanges[8];
    VP_STATUS status;
    ULONG i;

    (void)HwContext;
    (void)ArgumentString;

    *Again = 0;

    LogStr("NVX: HwVidFindAdapter entered\n");

    VideoPortZeroMemory(accessRanges, sizeof(accessRanges));

    status = VideoPortGetAccessRanges(
        HwDeviceExtension,
        0,
        NULL,
        8,
        accessRanges,
        NULL,
        NULL,
        &devExt->PciSlot
    );
    devExt->PciBusNumber=ConfigInfo->SystemIoBusNumber;

    LogStr("NVX: VideoPortGetAccessRanges status: ");
    LogHex(status);
    LogStr("\n");

    /* Ensure PCI Command Register has Memory Space & Bus Master enabled */
    {
        USHORT pciCmd = 0;
        VideoPortGetBusData(HwDeviceExtension, PCIConfiguration, 0, &pciCmd, 0x04, sizeof(pciCmd));
        LogStr("NVX: Initial PCI Command: 0x");
        LogHex(pciCmd);
        LogStr("\n");
        if ((pciCmd & 0x0006) != 0x0006) {
            pciCmd |= 0x0007; /* I/O + Memory + Bus Master */
            VideoPortSetBusData(HwDeviceExtension, PCIConfiguration, 0, &pciCmd, 0x04, sizeof(pciCmd));
            LogStr("NVX: Enabled PCI Memory & BusMastering: 0x");
            LogHex(pciCmd);
            LogStr("\n");
        }
    }

    for (i = 0; i < 8; i++) {
        if (accessRanges[i].RangeLength != 0) {
            LogStr("  Range: Start=");
            LogHex(accessRanges[i].RangeStart.QuadPart);
            LogStr(" Len=");
            LogHex(accessRanges[i].RangeLength);
            LogStr(" Io=");
            LogHex(accessRanges[i].RangeInIoSpace);
            LogStr("\n");
        }
    }

    /*
     * Identify BAR0 (MMIO <= 32MB) and BAR1 (VRAM aperture >= 64MB)
     */
    for (i = 0; i < 8; i++) {
        if (accessRanges[i].RangeLength == 0) continue;
        if (accessRanges[i].RangeInIoSpace == 0) {
            if (devExt->PhysicalBar0.QuadPart == 0 && accessRanges[i].RangeLength <= 0x2000000) {
                devExt->PhysicalBar0 = accessRanges[i].RangeStart;
                devExt->Bar0Length = accessRanges[i].RangeLength;
            } else if (devExt->PhysicalBar1.QuadPart == 0 && accessRanges[i].RangeLength >= 0x4000000) {
                devExt->PhysicalBar1 = accessRanges[i].RangeStart;
                devExt->Bar1Length = accessRanges[i].RangeLength;
            } else if (devExt->PhysicalInstBar.QuadPart==0) {
                devExt->PhysicalInstBar=accessRanges[i].RangeStart;
            }
        }
    }

    /* Fallback to known physical BARs if access ranges did not yield */
    if (devExt->PhysicalBar0.QuadPart == 0) {
        devExt->PhysicalBar0.QuadPart = 0xDE000000;
        devExt->Bar0Length = 0x1000000;
    }
    if (devExt->PhysicalBar1.QuadPart == 0) {
        devExt->PhysicalBar1.QuadPart = 0xFCC0000000ULL;
        devExt->Bar1Length = 0x10000000;
    }

    LogStr("NVX: Selected BAR0=");
    LogHex(devExt->PhysicalBar0.QuadPart);
    LogStr(" BAR1=");
    LogHex(devExt->PhysicalBar1.QuadPart);
    LogStr("\n");

    /*
     * Framebuffer display driver does not use interrupts.
     * Setting interrupt fields to 0 prevents videoprt from calling IoConnectInterrupt.
     */
    ConfigInfo->BusInterruptLevel = 0;
    ConfigInfo->BusInterruptVector = 0;

    /* HwInitialize builds this table from BIOS mode information. */
    devExt->NumModes = 0;
    devExt->CurrentModeIndex = 0;

    ConfigInfo->VdmPhysicalVideoMemoryAddress.QuadPart = 0;
    ConfigInfo->VdmPhysicalVideoMemoryLength = 0;

    LogStr("NVX: HwVidFindAdapter returning NO_ERROR\n");
    return NO_ERROR;
}

BOOLEAN HwVidInitialize(PVOID HwDeviceExtension) {
    PNVX_DEVICE_EXTENSION devExt = (PNVX_DEVICE_EXTENSION)HwDeviceExtension;

    LogStr("NVX: HwVidInitialize entered\n");

    /* Read real-mode information through VideoPort's INT10 service. Mapping
       physical address zero is not a substitute for its BIOS memory context. */
    if (!NvxVbeInitialize(HwDeviceExtension)) {
        LogStr("NVX: Cannot establish a verified VBE framebuffer; initialization failed.\n");
        return FALSE;
    }

    if (devExt->PhysicalBar0.QuadPart != 0 && devExt->Bar0Length != 0) {
        devExt->Bar0MmioVirtual = VideoPortGetDeviceBase(
            HwDeviceExtension,
            devExt->PhysicalBar0,
            devExt->Bar0Length,
            FALSE
        );
        LogStr("NVX: VideoPortGetDeviceBase BAR0 Virtual: ");
        LogHex((unsigned long long)devExt->Bar0MmioVirtual);
        LogStr("\n");

        if (devExt->Bar0MmioVirtual) {
            ULONG pmcBoot0 = VideoPortReadRegisterUlong((PULONG)devExt->Bar0MmioVirtual);
            LogStr("NVX: NV_PMC_BOOT_0 (GPU Arch): 0x");
            LogHex(pmcBoot0);
            LogStr("\n");
        }

#if NVX_ENABLE_EXPERIMENTAL_GSP
#if NVX_ENABLE_UNSAFE_GSP_PROBE
        if (!NvxGspInitSubsystem(HwDeviceExtension)) {
            LogStr("NVX [GSP]: Bootstrap incomplete; restoring verified VBE mode.\n");
            if (!NvxVbeSetMode(HwDeviceExtension, devExt->CurrentModeIndex))
                LogStr("NVX [GSP]: WARNING: VBE mode restore failed.\n");
        }
#else
        LogStr("NVX [GSP]: Validating and executing board FWSEC/FRTS before Booter Load.\n");
        NvxProbeFwsecRom(HwDeviceExtension,(PUCHAR)devExt->Bar0MmioVirtual);
        if (devExt->FwsecFrtsReady) {
            LogStr("NVX [GSP]: Verified FWSEC/FRTS prerequisite; starting GSP bootstrap.\n");
            if (!NvxGspInitSubsystem(HwDeviceExtension))
                LogStr("NVX [GSP]: Bootstrap incomplete; staging retained for diagnosis.\n");
        }
#endif
#else
        LogStr("NVX [GSP]: Experimental bootstrap disabled; preserving BIOS scanout.\n");
#endif
    }

    LogStr("NVX: HwVidInitialize returning TRUE\n");
    return TRUE;
}

BOOLEAN HwVidStartIO(
    PVOID HwDeviceExtension,
    PVIDEO_REQUEST_PACKET RequestPacket
) {
    PNVX_DEVICE_EXTENSION devExt = (PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    PSTATUS_BLOCK statusBlock = RequestPacket->StatusBlock;
    ULONG status = NO_ERROR;

    /* Serial polling per row can dominate desktop rendering. Acceleration
       paths already report sampled progress and all submission failures. */
    if(RequestPacket->IoControlCode!=IOCTL_NVX_ACCEL_UPLOAD &&
       RequestPacket->IoControlCode!=IOCTL_NVX_ACCEL_COPY &&
       RequestPacket->IoControlCode!=IOCTL_NVX_ACCEL_FILL) {
        LogStr("NVX: HwVidStartIO IoControlCode: ");
        LogHex(RequestPacket->IoControlCode);
        LogStr("\n");
    }

    statusBlock->Information = 0;

    switch (RequestPacket->IoControlCode) {
    case IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES: {
        PVIDEO_NUM_MODES numModes = (PVIDEO_NUM_MODES)RequestPacket->OutputBuffer;
        if (RequestPacket->OutputBufferLength < sizeof(VIDEO_NUM_MODES)) {
            status = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        numModes->NumModes = devExt->NumModes;
        numModes->ModeInformationLength = sizeof(VIDEO_MODE_INFORMATION);
        statusBlock->Information = sizeof(VIDEO_NUM_MODES);
        LogStr("NVX: IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES -> ");
        LogHex(devExt->NumModes);
        LogStr(" modes\n");
        break;
    }

    case IOCTL_VIDEO_QUERY_AVAIL_MODES: {
        ULONG requiredSize = devExt->NumModes * sizeof(VIDEO_MODE_INFORMATION);
        if (RequestPacket->OutputBufferLength < requiredSize) {
            status = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        VideoPortMoveMemory(RequestPacket->OutputBuffer, devExt->Modes, requiredSize);
        statusBlock->Information = requiredSize;
        LogStr("NVX: IOCTL_VIDEO_QUERY_AVAIL_MODES returned successfully\n");
        break;
    }

    case IOCTL_VIDEO_QUERY_CURRENT_MODE: {
        if (RequestPacket->OutputBufferLength < sizeof(VIDEO_MODE_INFORMATION)) {
            status = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        VideoPortMoveMemory(
            RequestPacket->OutputBuffer,
            &devExt->Modes[devExt->CurrentModeIndex],
            sizeof(VIDEO_MODE_INFORMATION)
        );
        statusBlock->Information = sizeof(VIDEO_MODE_INFORMATION);
        LogStr("NVX: IOCTL_VIDEO_QUERY_CURRENT_MODE index=");
        LogHex(devExt->CurrentModeIndex);
        LogStr("\n");
        break;
    }

    case IOCTL_VIDEO_SET_CURRENT_MODE: {
        PVIDEO_MODE mode = (PVIDEO_MODE)RequestPacket->InputBuffer;
        if (RequestPacket->InputBufferLength < sizeof(VIDEO_MODE)) {
            status = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        ULONG requested = mode->RequestedMode & 0x3fffffffUL; /* strip NO_ZERO_MEMORY/MAP_MEM_LINEAR flags */
        if (requested >= devExt->NumModes) {
            status = ERROR_INVALID_PARAMETER;
            break;
        }
        if (!NvxVbeSetMode(HwDeviceExtension, requested)) {
            LogStr("NVX: VBE mode change failed; attempting previous mode.\n");
            if (!NvxVbeSetMode(HwDeviceExtension, devExt->CurrentModeIndex))
                LogStr("NVX: Previous VBE mode could not be restored.\n");
            status = ERROR_INVALID_PARAMETER;
            break;
        }
        LogStr("NVX: Mode switched to index: ");
        LogHex(requested);
        LogStr("\n");
        break;
    }

    case IOCTL_VIDEO_MAP_VIDEO_MEMORY: {
        PVIDEO_MEMORY videoMemory = (PVIDEO_MEMORY)RequestPacket->InputBuffer;
        PVIDEO_MEMORY_INFORMATION memInfo = (PVIDEO_MEMORY_INFORMATION)RequestPacket->OutputBuffer;
        ULONG inIoSpace = 0;
        PVOID mappedVirtual = (videoMemory && RequestPacket->InputBufferLength >= sizeof(VIDEO_MEMORY)) ? videoMemory->RequestedVirtualAddress : NULL;
        ULONG length;
        if (devExt->CurrentModeIndex >= devExt->NumModes) {
            status = ERROR_INVALID_PARAMETER;
            break;
        }
        length = devExt->Modes[devExt->CurrentModeIndex].ScreenStride *
                 devExt->Modes[devExt->CurrentModeIndex].VisScreenHeight;

        if (RequestPacket->OutputBufferLength < sizeof(VIDEO_MEMORY_INFORMATION)) {
            status = ERROR_INSUFFICIENT_BUFFER;
            break;
        }

        status = VideoPortMapMemory(
            HwDeviceExtension,
            devExt->ModeFramebuffer[devExt->CurrentModeIndex],
            &length,
            &inIoSpace,
            &mappedVirtual
        );

        LogStr("NVX: Mapping scanout physical ");
        LogHex(devExt->ModeFramebuffer[devExt->CurrentModeIndex].QuadPart);
        LogStr(" stride ");LogHex(devExt->Modes[devExt->CurrentModeIndex].ScreenStride);
        LogStr(" bpp ");LogHex(devExt->ActiveBpp);LogStr("\n");
        LogStr("NVX: VideoPortMapMemory status: ");
        LogHex(status);
        LogStr(" Virtual: ");
        LogHex((unsigned long long)mappedVirtual);
        LogStr("\n");

        if (status == NO_ERROR) {
            memInfo->VideoRamBase = mappedVirtual;
            memInfo->VideoRamLength = length;
            memInfo->FrameBufferBase = mappedVirtual;
            memInfo->FrameBufferLength = length;
            devExt->FramebufferVirtual = mappedVirtual;
            (void)NvxGspDiscoverScanout(HwDeviceExtension,devExt->CurrentModeIndex);
            (void)NvxGspInitDisplayChannels(HwDeviceExtension);
            statusBlock->Information = sizeof(VIDEO_MEMORY_INFORMATION);
        }
        break;
    }

    case IOCTL_VIDEO_UNMAP_VIDEO_MEMORY: {
        PVIDEO_MEMORY videoMemory = (PVIDEO_MEMORY)RequestPacket->InputBuffer;
        if (RequestPacket->InputBufferLength < sizeof(VIDEO_MEMORY)) {
            status = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        status = VideoPortUnmapMemory(
            HwDeviceExtension,
            videoMemory->RequestedVirtualAddress,
            0
        );
        devExt->GspScanoutMapped=FALSE;
        break;
    }

    case IOCTL_VIDEO_RESET_DEVICE:
    case IOCTL_VIDEO_SET_COLOR_REGISTERS:
        status = NO_ERROR;
        break;

    case IOCTL_NVX_ACCEL_COPY: {
        PNVX_ACCEL_COPY_PARAMS params = (PNVX_ACCEL_COPY_PARAMS)RequestPacket->InputBuffer;
        static ULONG s_copyCount = 0;
        if (!params || RequestPacket->InputBufferLength < sizeof(NVX_ACCEL_COPY_PARAMS)) {
            status = ERROR_INVALID_PARAMETER;
            break;
        }
        if (!devExt->GspCopyVerified) {
            status = ERROR_NOT_SUPPORTED;
            break;
        }
        if (NvxGspSubmitCopy(HwDeviceExtension, params->DstGpuVa, params->SrcGpuVa, params->Bytes)) {
            status = NO_ERROR;
            statusBlock->Information = sizeof(NVX_ACCEL_COPY_PARAMS);
            s_copyCount++;
            if ((s_copyCount <= 5) || (s_copyCount % 100 == 0)) {
                LogStr("NVX [ACCEL]: GPU Copy #"); LogHex(s_copyCount);
                LogStr(" bytes "); LogHex(params->Bytes); LogStr("\n");
            }
        } else {
            status = ERROR_DEVICE_NOT_READY;
        }
        break;
    }

    case IOCTL_NVX_ACCEL_FILL: {
        PNVX_ACCEL_FILL_PARAMS params = (PNVX_ACCEL_FILL_PARAMS)RequestPacket->InputBuffer;
        static ULONG s_fillCount = 0;
        if (!params || RequestPacket->InputBufferLength < sizeof(NVX_ACCEL_FILL_PARAMS)) {
            status = ERROR_INVALID_PARAMETER;
            break;
        }
        if (!devExt->GspCopyVerified) {
            status = ERROR_NOT_SUPPORTED;
            break;
        }
        if (NvxGspSubmitFill(HwDeviceExtension, params->DstGpuVa, params->Color, params->Bytes)) {
            status = NO_ERROR;
            statusBlock->Information = sizeof(NVX_ACCEL_FILL_PARAMS);
            s_fillCount++;
            if ((s_fillCount <= 5) || (s_fillCount % 100 == 0)) {
                LogStr("NVX [ACCEL]: GPU Fill #"); LogHex(s_fillCount);
                LogStr(" color "); LogHex(params->Color);
                LogStr(" bytes "); LogHex(params->Bytes); LogStr("\n");
            }
        } else {
            status = ERROR_DEVICE_NOT_READY;
        }
        break;
    }

    case IOCTL_NVX_ACCEL_UPLOAD: {
        PNVX_ACCEL_UPLOAD_PARAMS params=(PNVX_ACCEL_UPLOAD_PARAMS)RequestPacket->InputBuffer;
        static ULONG s_uploadKinds[4]={0,0,0,0};
        static ULONG s_fbCompareCount=0;
        ULONG headerBytes=FIELD_OFFSET(NVX_ACCEL_UPLOAD_PARAMS,Data);
        ULONG required;
        ULONG nonzeroPixels=0, pixelTransitions=0, payloadHash=2166136261UL;
        if(!params||RequestPacket->InputBufferLength<headerBytes) {
            status=ERROR_INVALID_PARAMETER;
            break;
        }
        if(params->Reserved>NVX_UPLOAD_KIND_TEXTOUT||
           params->Bytes==0||params->Bytes>GSP_UPLOAD_MAX_BYTES||
           params->Bytes>((ULONG)~0U)-headerBytes) {
            status=ERROR_INVALID_PARAMETER;
            break;
        }
        required=headerBytes+params->Bytes;
        if(RequestPacket->InputBufferLength!=required||!devExt->GspCopyVerified) {
            status=ERROR_NOT_SUPPORTED;
            break;
        }
        if(params->Reserved==NVX_UPLOAD_KIND_TEXTOUT) {
            ULONG i, previous=0;
            for(i=0;i<params->Bytes;i++) {
                payloadHash^=params->Data[i];
                payloadHash*=16777619UL;
            }
            for(i=0;i+sizeof(ULONG)<=params->Bytes;i+=sizeof(ULONG)) {
                ULONG pixel=((PULONG)params->Data)[i/sizeof(ULONG)];
                if(pixel!=0) nonzeroPixels++;
                if(i&&pixel!=previous) pixelTransitions++;
                previous=pixel;
            }
        }
        if(NvxGspUpload(HwDeviceExtension,params->DstGpuVa,params->Data,params->Bytes)) {
            status=NO_ERROR;
            statusBlock->Information=required;
            s_uploadKinds[params->Reserved]++;
            /* Compare early text uploads through the CPU framebuffer mapping.
             * The copy engine fence in NvxGspUpload has completed at this point.
             */
            if(params->Reserved==NVX_UPLOAD_KIND_TEXTOUT && s_fbCompareCount<3 &&
               devExt->FramebufferVirtual && devExt->CurrentModeIndex<devExt->NumModes &&
               params->DstGpuVa>=devExt->GspVramGpuVa) {
                ULONG64 fbOffset=params->DstGpuVa-devExt->GspVramGpuVa;
                VIDEO_MODE_INFORMATION *mode=&devExt->Modes[devExt->CurrentModeIndex];
                ULONG64 fbBytes=(ULONG64)mode->ScreenStride*mode->VisScreenHeight;
                if(fbOffset<=fbBytes && params->Bytes<=fbBytes-fbOffset) {
                    volatile UCHAR *fb=(volatile UCHAR *)devExt->FramebufferVirtual+(ULONG)fbOffset;
                    ULONG i, mismatches=0;
                    for(i=0;i<params->Bytes;i++) {
                        if(fb[i]!=params->Data[i]) mismatches++;
                    }
                    s_fbCompareCount++;
                    LogStr("NVX [ACCEL]: FB readback #");LogHex(s_fbCompareCount);
                    LogStr(" dst=" );LogHex(params->DstGpuVa);
                    LogStr(" offset=");LogHex((ULONG)fbOffset);
                    LogStr(" bytes=");LogHex(params->Bytes);
                    LogStr(" mismatches=");LogHex(mismatches);LogStr("\n");
                }
            }
            if((s_uploadKinds[params->Reserved]<=3)||(s_uploadKinds[params->Reserved]%100==0)) {
                LogStr("NVX [ACCEL]: GPU Upload kind=");LogHex(params->Reserved);
                LogStr(" #");LogHex(s_uploadKinds[params->Reserved]);
                LogStr(" dst=");LogHex(params->DstGpuVa);
                LogStr(" bytes=");LogHex(params->Bytes);
                if(params->Reserved==NVX_UPLOAD_KIND_TEXTOUT) {
                    LogStr(" nonzeroPixels=");LogHex(nonzeroPixels);
                    LogStr(" transitions=");LogHex(pixelTransitions);
                    LogStr(" hash=");LogHex(payloadHash);
                }
                LogStr("\n");
            }
        } else {
            status=ERROR_DEVICE_NOT_READY;
        }
        break;
    }

    default:
        status = ERROR_INVALID_FUNCTION;
        break;
    }

    statusBlock->Status = status;
    return TRUE;
}

typedef LONG NTSTATUS;
typedef NTSTATUS (*PDRIVER_ADD_DEVICE)(PVOID DriverObject, PVOID PhysicalDeviceObject);
typedef NTSTATUS (*PDRIVER_DISPATCH)(PVOID DeviceObject, PVOID Irp);

static PDRIVER_ADD_DEVICE RealAddDevice = NULL;
static PDRIVER_DISPATCH RealPnpDispatch = NULL;
static PDRIVER_DISPATCH RealPowerDispatch = NULL;

static NTSTATUS HookedAddDevice(PVOID DriverObject, PVOID PhysicalDeviceObject) {
    LogStr("NVX: HookedAddDevice called! DrvObj=");
    LogHex((unsigned long long)DriverObject);
    LogStr(" PDO=");
    LogHex((unsigned long long)PhysicalDeviceObject);
    LogStr("\n");

    NTSTATUS status = RealAddDevice(DriverObject, PhysicalDeviceObject);

    LogStr("NVX: RealAddDevice returned: ");
    LogHex((unsigned long long)status);
    LogStr("\n");
    return status;
}

static NTSTATUS HookedPnpDispatch(PVOID DeviceObject, PVOID Irp) {
    unsigned char *irpBytes = (unsigned char *)Irp;
    unsigned char **stackLocationPtr = (unsigned char **)(irpBytes + 0xb8);
    unsigned char *stack = *stackLocationPtr;
    unsigned char minor = stack ? stack[1] : 0xFF;

    LogStr("NVX: HookedPnpDispatch! Minor=0x");
    LogHex(minor);
    LogStr(" DevObj=");
    LogHex((unsigned long long)DeviceObject);
    LogStr("\n");

    NTSTATUS status = RealPnpDispatch(DeviceObject, Irp);

    LogStr("NVX: RealPnpDispatch returned: ");
    LogHex((unsigned long long)status);
    LogStr("\n");
    return status;
}

static NTSTATUS HookedPowerDispatch(PVOID DeviceObject, PVOID Irp) {
    LogStr("NVX: HookedPowerDispatch!\n");
    NTSTATUS status = RealPowerDispatch(DeviceObject, Irp);
    return status;
}

ULONG DriverEntry(PVOID Context1, PVOID Context2) {
    VIDEO_HW_INITIALIZATION_DATA hwInitData;
    ULONG status;

    LogStr("\n========================================\n");
    LogStr(" NVX Video Miniport Driver 1.0.0.136 (C67D core initialization)\n");
    LogStr("========================================\n");

    VideoPortZeroMemory(&hwInitData, sizeof(VIDEO_HW_INITIALIZATION_DATA));

    hwInitData.HwInitDataSize = sizeof(VIDEO_HW_INITIALIZATION_DATA);
    hwInitData.AdapterInterfaceType = PCIBus;
    hwInitData.HwFindAdapter = HwVidFindAdapter;
    hwInitData.HwInitialize = HwVidInitialize;
    hwInitData.HwInterrupt = HwVidInterrupt;
    hwInitData.HwStartIO = HwVidStartIO;
    hwInitData.HwResetHw = HwVidResetHw;
    hwInitData.HwDeviceExtensionSize = sizeof(NVX_DEVICE_EXTENSION);
    hwInitData.StartingDeviceNumber = 0;

    hwInitData.HwSetPowerState = HwVidSetPowerState;
    hwInitData.HwGetPowerState = HwVidGetPowerState;
    hwInitData.HwGetVideoChildDescriptor = HwVidGetVideoChildDescriptor;

    status = VideoPortInitialize(Context1, Context2, &hwInitData, NULL);
    LogStr("NVX: VideoPortInitialize returned: ");
    LogHex(status);
    LogStr("\n");

    if (status == 0) {
        unsigned char *drvObj = (unsigned char *)Context1;
        unsigned char *drvExt = *(unsigned char **)(drvObj + 0x30);
        if (drvExt) {
            RealAddDevice = *(PDRIVER_ADD_DEVICE *)(drvExt + 0x08);
            LogStr("NVX: Original AddDevice: ");
            LogHex((unsigned long long)RealAddDevice);
            LogStr("\n");
            *(PDRIVER_ADD_DEVICE *)(drvExt + 0x08) = HookedAddDevice;

            /*
             * videoprt.sys VpIsMultiFunctionSupported (at offset 0x25ad0) checks if the GPU
             * is a multi-function PCI device (bit 7 of PCI HeaderType 0x0E).
             * For RTX 3070 (display + audio), it queries MultiFunctionSupported from the registry,
             * and if absent or 0, returns FALSE (0), causing IRP_MN_START_DEVICE to fail with
             * STATUS_UNSUCCESSFUL (0xC0000001 / Code 10) before calling HwVidFindAdapter!
             * Patch VpIsMultiFunctionSupported to unconditionally return TRUE (1).
             */
            if (RealAddDevice) {
                unsigned char *vpBase = (unsigned char *)RealAddDevice - 0x280e0;
                unsigned char *multiFunc = vpBase + 0x25ad0;
                if (multiFunc[0] == 0x4c && multiFunc[1] == 0x8b && multiFunc[2] == 0xdc) {
                    unsigned long long cr0;
                    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
                    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0 & ~(1ULL << 16)));

                    /* mov $1, %eax; ret */
                    multiFunc[0] = 0xb8;
                    multiFunc[1] = 0x01;
                    multiFunc[2] = 0x00;
                    multiFunc[3] = 0x00;
                    multiFunc[4] = 0x00;
                    multiFunc[5] = 0xc3;

                    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0));
                    LogStr("NVX: Patched VpIsMultiFunctionSupported at ");
                    LogHex((unsigned long long)multiFunc);
                    LogStr("\n");
                }
            }
        }

        RealPnpDispatch = *(PDRIVER_DISPATCH *)(drvObj + 0x148);
        LogStr("NVX: Original PnpDispatch: ");
        LogHex((unsigned long long)RealPnpDispatch);
        LogStr("\n");
        *(PDRIVER_DISPATCH *)(drvObj + 0x148) = HookedPnpDispatch;

        RealPowerDispatch = *(PDRIVER_DISPATCH *)(drvObj + 0x120);
        *(PDRIVER_DISPATCH *)(drvObj + 0x120) = HookedPowerDispatch;
    }

    return status;
}
