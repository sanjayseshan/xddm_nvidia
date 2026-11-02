/*
 * NVIDIA GeForce RTX 3070 (Ampere GA104) XDDM Video Miniport Driver
 * Target: Windows XP Professional x64 Edition / Server 2003 x64 (NT 5.2)
 *
 * Driver Version: 1.0.0.42
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
static UCHAR NvxFwsecDryRunImage[0x10000];

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

static VOID NvxDryRunFwsecStage(NVX_PROM_CONTEXT *prom,const NVX_FWSEC_INFO *info,
                                const NVX_FRTS_LAYOUT *layout,uint32_t signatureOffset)
{
    uint32_t sigDst,mapper,cmd,baseCrc,stagedCrc,sigCrc;
    PUCHAR image=NvxFwsecDryRunImage;
    if (!prom||!info||!layout||!info->StoredSize||info->StoredSize>sizeof(NvxFwsecDryRunImage) ||
        info->ImemLoadSize>info->StoredSize ||
        info->PkcDataOffset>info->DmemLoadSize ||
        !NvxPromCopy(prom,info->ImageOffset,image,info->StoredSize)) {
        LogStr("NVX [FWSEC-STAGE]: invalid image bounds/read; execution blocked.\n");return;
    }
    baseCrc=NvxCrc32(image,info->StoredSize);
    sigDst=info->ImemLoadSize+info->PkcDataOffset;
    mapper=info->ImemLoadSize+info->DmemMapperOffset;
    cmd=info->ImemLoadSize+info->CommandInOffset;
    if (sigDst>info->StoredSize || 384U>info->StoredSize-sigDst ||
        mapper>info->StoredSize || 64U>info->StoredSize-mapper ||
        cmd>info->StoredSize || 44U>info->StoredSize-cmd ||
        !NvxPromCopy(prom,signatureOffset,image+sigDst,384U)) {
        LogStr("NVX [FWSEC-STAGE]: patch bounds/read invalid; execution blocked.\n");
        VideoPortZeroMemory(image,sizeof(NvxFwsecDryRunImage));return;
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
    LogStr("NVX [FWSEC-STAGE]: base_crc=");LogHex(baseCrc);
    LogStr(" expected=");LogHex(info->ImageCrc32);
    LogStr(" selected_sig_crc=");LogHex(sigCrc);
    LogStr(" staged_crc=");LogHex(stagedCrc);LogStr("\n");
    LogStr("NVX [FWSEC-STAGE]: sig_dmem=");LogHex(info->PkcDataOffset);
    LogStr(" mapper_dmem=");LogHex(info->DmemMapperOffset);
    LogStr(" init_cmd=0x15 cmd_dmem=");LogHex(info->CommandInOffset);
    LogStr(" bytes=0x2C dry-run complete; no GPU writes.\n");
    VideoPortZeroMemory(image,sizeof(NvxFwsecDryRunImage));
}

static VOID NvxProbeFwsecRom(PUCHAR bar0)
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
            NvxDryRunFwsecStage(&prom,&info,&frtsLayout,selectedSignatureOffset);
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
    (void)HwDeviceExtension;
    (void)pChildDescriptor;
    (void)pUnused;

    LogStr("NVX: HwVidGetVideoChildDescriptor ChildIndex: ");
    LogHex(ChildEnumInfo->ChildIndex);
    LogStr("\n");

    if (ChildEnumInfo->ChildIndex == 0) {
        *VideoChildType = Monitor;
        *UId = 1;
        return VIDEO_ENUM_MORE_DEVICES;
    }

    return VIDEO_ENUM_NO_MORE_DEVICES;
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
        NULL
    );

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
        LogStr("NVX [GSP]: Hardware bootstrap gated: board VBIOS FWSEC/FRTS is required before Booter Load.\n");
        NvxProbeFwsecRom((PUCHAR)devExt->Bar0MmioVirtual);
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

    LogStr("NVX: HwVidStartIO IoControlCode: ");
    LogHex(RequestPacket->IoControlCode);
    LogStr("\n");

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
        break;
    }

    case IOCTL_VIDEO_RESET_DEVICE:
    case IOCTL_VIDEO_SET_COLOR_REGISTERS:
        status = NO_ERROR;
        break;

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
    LogStr(" NVX Video Miniport Driver 1.0.0.42\n");
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
