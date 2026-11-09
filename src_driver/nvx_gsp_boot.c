/*
 * NVIDIA Ampere GA104 GSP Bootloader & Sequencer Subsystem
 */

static BOOLEAN NvxWait(PUCHAR bar0, ULONG reg, ULONG mask, ULONG expected,
                       ULONG iterations, ULONG delay)
{
    while (iterations--) {
        ULONG value = VideoPortReadRegisterUlong((PULONG)(bar0 + reg));
        if (value != 0xffffffff && (value & mask) == expected) return TRUE;
        VideoPortStallExecution(delay);
    }
    LogStr("NVX [GSP]: Timeout at register "); LogHex(reg);
    LogStr(" value "); LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+reg)));
    LogStr("\n");
    return FALSE;
}

int NvxGspSequencerCore(void *context,unsigned int opcode)
{
    PNVX_DEVICE_EXTENSION dev=context;
    PUCHAR bar0;
    ULONG value,i;
    if (!dev||!dev->Bar0MmioVirtual||!dev->GspDmaVirtual||!dev->GspBootVerified||
        opcode<5||opcode>8) return FALSE;
    bar0=dev->Bar0MmioVirtual;
    LogStr("NVX [SEQ-CORE]: opcode=");LogHex(opcode);LogStr("\n");
    if (opcode==5||opcode==8) {
        for(i=0;i<150;i++) {
            if(VideoPortReadRegisterUlong((PULONG)(bar0+0x1100f4))&0x80000000U) break;
            VideoPortStallExecution(1);
        }
        value=VideoPortReadRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_ENGINE));
        VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_ENGINE),value|1U);
        for(i=0;i<10;i++) (void)VideoPortReadRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_ENGINE));
        VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_ENGINE),value&~1U);
        for(i=0;i<10;i++) (void)VideoPortReadRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_ENGINE));
        if(!NvxWait(bar0,0x1100f4,0x1000,0,2000,10)) return FALSE;
    }
    if(opcode==5) {
        value=VideoPortReadRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_BCR));
        if(value&0x10) {
            VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_BCR),0);
            if(!NvxWait(bar0,NV_PGSP_FALCON_BCR,1,1,20000,5)) return FALSE;
        }
        VideoPortWriteRegisterUlong((PULONG)(bar0+0x110084),VideoPortReadRegisterUlong((PULONG)bar0));
        value=VideoPortReadRegisterUlong((PULONG)(bar0+0x110624));
        VideoPortWriteRegisterUlong((PULONG)(bar0+0x110624),value|0x80);
        VideoPortWriteRegisterUlong((PULONG)(bar0+0x11010c),0);
        return TRUE;
    }
    if(opcode==6) {
        value=VideoPortReadRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_CPUCTL));
        VideoPortWriteRegisterUlong((PULONG)(bar0+(value&0x40?NV_PGSP_FALCON_CPUCTL_ALIAS:NV_PGSP_FALCON_CPUCTL)),2);
        return TRUE;
    }
    if(opcode==7) return NvxWait(bar0,NV_PGSP_FALCON_CPUCTL,0x10,0x10,40000,50);
    /* GA102 CORE_RESUME: select RISC-V, restore LibOS argument pointer,
       restart the suspended SEC2 RTOS, and verify stage-3 handoff. */
    VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_BCR),0x111);
    {
        ULONG64 args=dev->GspDmaPhysical.QuadPart+GSP_LIBOS_ARGS_OFFSET;
        VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_MAILBOX0),(ULONG)args);
        VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_MAILBOX1),(ULONG)(args>>32));
    }
    value=VideoPortReadRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_CPUCTL));
    VideoPortWriteRegisterUlong((PULONG)(bar0+(value&0x40?NV_PSEC_FALCON_CPUCTL_ALIAS:NV_PSEC_FALCON_CPUCTL)),2);
    if(!NvxWait(bar0,0x1180f8,0x04000000,0x04000000,40000,50)||
       VideoPortReadRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_MAILBOX0))) return FALSE;
    VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_OS),0);
    return (VideoPortReadRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_RISCV_STAT))&0x80)!=0;
}

static int NvxSeqRead(void *context,uint32_t address,uint32_t *value)
{
    PNVX_DEVICE_EXTENSION dev=context;
    if(!dev||!value||(address&3)||dev->Bar0Length<4||address>dev->Bar0Length-4) return 0;
    *value=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+address));return 1;
}
static int NvxSeqWrite(void *context,uint32_t address,uint32_t value)
{
    PNVX_DEVICE_EXTENSION dev=context;
    if(!dev||(address&3)||dev->Bar0Length<4||address>dev->Bar0Length-4) return 0;
    VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+address),value);return 1;
}
static int NvxSeqDelay(void *context,uint32_t microseconds)
{
    (void)context;
    while(microseconds) {
        ULONG step=microseconds>50?50:microseconds;
        VideoPortStallExecution(step);microseconds-=step;
    }
    return 1;
}

/*
 * GA102 Falcon enable sequence.  This follows gp102_flcn_reset_eng(),
 * ga102_flcn_select(), and gm200_flcn_enable() instead of overwriting the
 * entire ENGINE register.  The latter is important on a GPU handed to us by
 * the VGA firmware, where unrelated control bits can already be populated.
 */
static BOOLEAN NvxSec2ResetEnable(PUCHAR bar0)
{
    ULONG value;
    ULONG timeout;

    /* GA102 reset preparation is advisory in Nouveau, with a 150us window. */
    (void)VideoPortReadRegisterUlong((PULONG)(bar0 + 0x8400f4));
    timeout = 150;
    while (timeout--) {
        if (VideoPortReadRegisterUlong((PULONG)(bar0 + 0x8400f4)) & 0x80000000)
            break;
        VideoPortStallExecution(1);
    }

    value = VideoPortReadRegisterUlong((PULONG)(bar0 + NV_PSEC_FALCON_ENGINE));
    VideoPortWriteRegisterUlong((PULONG)(bar0 + NV_PSEC_FALCON_ENGINE), value | 1);
    VideoPortStallExecution(10);
    VideoPortWriteRegisterUlong((PULONG)(bar0 + NV_PSEC_FALCON_ENGINE), value & ~1UL);

    if (!NvxWait(bar0, 0x8400f4, 0x00001000, 0, 2000, 10))
        return FALSE;

    /* Select the Falcon core when the second PRI block currently selects RISCV. */
    value = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x841668));
    if (value & 0x10) {
        VideoPortWriteRegisterUlong((PULONG)(bar0 + 0x841668), 0);
        if (!NvxWait(bar0, 0x841668, 1, 1, 1000, 10))
            return FALSE;
    }

    /* MC enable is dynamically described by TOP on Ampere.  Firmware normally
       leaves SEC2 enabled; record its state rather than guessing a PMC mask. */
    LogStr("NVX [SEC2]: MC_ENABLE 0x000600: 0x");
    LogHex(VideoPortReadRegisterUlong((PULONG)(bar0 + 0x000600)));
    LogStr(" BCR: 0x");
    LogHex(VideoPortReadRegisterUlong((PULONG)(bar0 + 0x841668)));
    LogStr("\n");

    if (!NvxWait(bar0, 0x8400f4, 0x00001000, 0, 2000, 10))
        return FALSE;

    VideoPortWriteRegisterUlong((PULONG)(bar0 + 0x840084),
                                VideoPortReadRegisterUlong((PULONG)bar0));
    return TRUE;
}

static VOID NvxLogSec2State(PUCHAR bar0, const char *where)
{
    LogStr("NVX [SEC2]: "); LogStr(where);
    LogStr(" HWCFG2="); LogHex(VideoPortReadRegisterUlong((PULONG)(bar0 + 0x8400f4)));
    LogStr(" CPUCTL="); LogHex(VideoPortReadRegisterUlong((PULONG)(bar0 + NV_PSEC_FALCON_CPUCTL)));
    LogStr(" ALIAS="); LogHex(VideoPortReadRegisterUlong((PULONG)(bar0 + NV_PSEC_FALCON_CPUCTL_ALIAS)));
    LogStr(" BOOTVEC="); LogHex(VideoPortReadRegisterUlong((PULONG)(bar0 + NV_PSEC_FALCON_BOOTVEC)));
    LogStr(" DMACTL="); LogHex(VideoPortReadRegisterUlong((PULONG)(bar0 + 0x84010c)));
    LogStr(" DMACMD="); LogHex(VideoPortReadRegisterUlong((PULONG)(bar0 + NV_PSEC_FALCON_DMATRFCMD)));
    LogStr(" FBIF_CTL="); LogHex(VideoPortReadRegisterUlong((PULONG)(bar0 + 0x840624)));
    LogStr("\n");
}

static BOOLEAN NvxSec2Dma(PUCHAR bar0, ULONG64 physical, ULONG source,
                          ULONG length, BOOLEAN imem)
{
    ULONG offset;
    /* IMEM tags derive from the source offset; physical TCM starts at zero. */
    if (!imem) physical += source;
    VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_DMATRFBASE), (ULONG)(physical>>8));
    VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_DMATRFBASE1), (ULONG)(physical>>40));
    for (offset=0; offset<length; offset+=256) {
        VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_DMATRFMOFF), offset);
        VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_DMATRFFBOFF), offset+(imem ? source : 0));
        VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_DMATRFCMD), imem ? 0x614 : 0x600);
        /* Bit 2 is SEC, not BUSY. Completion is bit 1, for every transfer. */
        if (!NvxWait(bar0,NV_PSEC_FALCON_DMATRFCMD,2,2,20000,5)) return FALSE;
    }
    return TRUE;
}

BOOLEAN NvxGspInitSubsystem(PVOID HwDeviceExtension) {
    PNVX_DEVICE_EXTENSION devExt = (PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    PUCHAR bar0 = (PUCHAR)devExt->Bar0MmioVirtual;
    ULONG bootloaderLen = src_driver_bootloader_535_113_01_bin_len;
    ULONG booterLoadLen = src_driver_booter_load_535_113_01_bin_len;
    ULONG alignedLen = (bootloaderLen + 0xFF) & ~0xFF;
    ULONG timeout;
    VP_DEVICE_DESCRIPTION devDesc;
    NVX_BOOTER_INFO bt;
    int signatureIndex;
    uint32_t imageOffset, imageSize, signatureOffset, signatureSize;
    ULONG64 firmwareSize = (ULONG64)(_binary_dist_full_gsp_ga104_bin_end -
                                     _binary_dist_full_gsp_ga104_bin_start);
    /* A failed engine may still reference staging memory. Do not replace it. */
    if (devExt->GspDmaVirtual) return devExt->GspInitialized;
    devExt->GspInitialized = FALSE;
    devExt->GspBootVerified = FALSE;

    LogStr("NVX [GSP]: Initializing GA104 GSP Subsystem with authentic binary blobs...\n");
    VideoPortDebugPrint(Info, "NVX [GSP]: Initializing GA104 GSP Subsystem with authentic binary blobs...\n");

    if (!bar0) {
        LogStr("NVX [GSP]: BAR0 MMIO virtual mapping is NULL! Aborting.\n");
        return FALSE;
    }

    LogStr("NVX [GSP]: Embedded Falcon Bootloader size: ");
    LogHex(bootloaderLen);
    LogStr(" bytes (aligned ");
    LogHex(alignedLen);
    LogStr(")\n");
    LogStr("NVX [GSP]: Embedded Booter Load size: ");
    LogHex(booterLoadLen);
    LogStr(" bytes\n");

    /* Reject malformed/mismatched containers before DMA allocation or engine writes. */
    if (!NvxParseBooter(src_driver_booter_load_535_113_01_bin, booterLoadLen, &bt) ||
        !NvxFindElfSection(_binary_dist_full_gsp_ga104_bin_start, firmwareSize,
                          ".fwimage", &imageOffset, &imageSize) ||
        !NvxFindElfSection(_binary_dist_full_gsp_ga104_bin_start, firmwareSize,
                          ".fwsignature_ga10x", &signatureOffset, &signatureSize) ||
        imageSize != GSP_FW_IMAGE_SIZE || signatureSize != GSP_SIG_SIZE ||
        bt.DataSize > GSP_WPR_META_OFFSET-GSP_BOOTER_LOAD_OFFSET ||
        bt.EngineId != 1 || bt.UcodeId > 16) {
        LogStr("NVX [GSP]: Invalid/unsupported firmware container.\n");
        return FALSE;
    }
    signatureIndex = NvxSelectSignature(VideoPortReadRegisterUlong(
        (PULONG)(bar0+0x824140+(bt.UcodeId-1)*4)), bt.FuseVersion, bt.SignatureCount);
    if (signatureIndex < 0) {
        LogStr("NVX [SEC2]: No signature matches GPU fuse version.\n");
        return FALSE;
    }
    LogStr("NVX [SEC2]: Selected signature index "); LogHex(signatureIndex); LogStr("\n");

    /* Obtain DMA adapter from VideoPort */
    VideoPortZeroMemory(&devDesc, sizeof(devDesc));
    devDesc.ScatterGather = TRUE;
    /* XP's VideoPort VP_DEVICE_DESCRIPTION has no MaximumPhysicalAddress
       member. Request 32-bit DMA and explicitly decline 64-bit DMA instead. */
    devDesc.Dma32BitAddresses = TRUE;
    devDesc.Dma64BitAddresses = FALSE;
    devDesc.MaximumLength = 0x3000000; /* 48 MB */

    devExt->DmaAdapter = VideoPortGetDmaAdapter(HwDeviceExtension, &devDesc);
    LogStr("NVX [GSP]: VideoPortGetDmaAdapter returned: 0x");
    LogHex((unsigned long long)devExt->DmaAdapter);
    LogStr("\n");

    if (!devExt->DmaAdapter) return FALSE;

    /* Allocate 48MB contiguous coherent DMA memory for GSP staging, Radix-3 table, and queues */
    devExt->GspDmaLength = 0x3000000; /* 48 MB */
    devExt->GspDmaVirtual = VideoPortAllocateCommonBuffer(
        HwDeviceExtension,
        devExt->DmaAdapter,
        devExt->GspDmaLength,
        &devExt->GspDmaPhysical,
        FALSE,
        NULL
    );

    if (!devExt->GspDmaVirtual) {
        LogStr("NVX [GSP]: Failed to allocate contiguous DMA buffer for GSP (48MB)!\n");
        VideoPortDebugPrint(Error, "NVX [GSP]: Failed to allocate contiguous DMA buffer for GSP (%d bytes)\n", devExt->GspDmaLength);
        VideoPortPutDmaAdapter(HwDeviceExtension, devExt->DmaAdapter);
        devExt->DmaAdapter = NULL;
        return FALSE;
    }
    LogStr("NVX [GSP-DMA]: common buffer physical=");
    LogHex((ULONG64)devExt->GspDmaPhysical.QuadPart);
    LogStr(" end=");
    LogHex((ULONG64)devExt->GspDmaPhysical.QuadPart + devExt->GspDmaLength);
    LogStr(" requested 32-bit=1 64-bit=0 cpu-cache=disabled\n");
    if ((devExt->GspDmaPhysical.QuadPart & 0xfff) ||
        (ULONG64)devExt->GspDmaPhysical.QuadPart + devExt->GspDmaLength > 0x100000000ULL) {
        LogStr("NVX [GSP]: VideoPort violated the requested 32-bit DMA range or alignment; GSP remains disabled.\n");
        NvxGspShutdown(HwDeviceExtension);
        return FALSE;
    }

    /* Parse authentic NVFW container header (Ported from Linux Nouveau nvfw_bin_hdr) */
    PNVFW_BIN_HDR blHdr = (PNVFW_BIN_HDR)src_driver_bootloader_535_113_01_bin;
    PRM_RISCV_UCODE_DESC blDesc = NULL;
    ULONG blCodeOffset = 0;
    ULONG blCodeSize = bootloaderLen;

    if (blHdr->BinMagic == 0x10DE) {
        LogStr("NVX [GSP]: Authentic NVFW container header detected!\n");
        LogStr("NVX [GSP]:   BinVer: 0x"); LogHex(blHdr->BinVer);
        LogStr(" HdrOff: 0x"); LogHex(blHdr->HeaderOffset);
        LogStr(" DataOff: 0x"); LogHex(blHdr->DataOffset);
        LogStr(" DataSize: 0x"); LogHex(blHdr->DataSize);
        LogStr("\n");
        blCodeOffset = blHdr->DataOffset;
        blCodeSize = blHdr->DataSize;
        if (blHdr->HeaderOffset <= bootloaderLen - sizeof(RM_RISCV_UCODE_DESC)) {
            blDesc = (PRM_RISCV_UCODE_DESC)((PUCHAR)src_driver_bootloader_535_113_01_bin + blHdr->HeaderOffset);
            LogStr("NVX [GSP]:   RISC-V AppVersion: 0x"); LogHex(blDesc->AppVersion);
            LogStr(" MonitorCodeOff: 0x"); LogHex(blDesc->MonitorCodeOffset);
            LogStr(" ManifestOff: 0x"); LogHex(blDesc->ManifestOffset);
            LogStr("\n");
        }
    }

    if (!blDesc || blHdr->BinVer != 1 || blCodeOffset > bootloaderLen ||
        blCodeSize > bootloaderLen-blCodeOffset ||
        blCodeSize > GSP_BOOTER_LOAD_OFFSET-GSP_BOOTLOADER_OFFSET ||
        blDesc->MonitorCodeOffset >= blCodeSize || blDesc->MonitorDataOffset >= blCodeSize ||
        blDesc->ManifestOffset >= blCodeSize) {
        LogStr("NVX [GSP]: Invalid bootloader descriptor.\n");
        NvxGspShutdown(HwDeviceExtension);
        return FALSE;
    }
    ULONG btCodeOffset = bt.DataOffset;
    ULONG btCodeSize = bt.DataSize;

    /* Zero entire 48MB common buffer */
    VideoPortZeroMemory(devExt->GspDmaVirtual, devExt->GspDmaLength);

    /*
     * STAGE 1: Copy authentic GSP Firmware Image (.fwimage section from gsp_ga104.bin)
     * Size: 38,043,648 bytes (0x2448000), 9288 4KB pages.
     */
    LogStr("NVX [GSP]: Staging authentic GSP .fwimage (36.28MB) at DMA offset 0x0...\n");
    VideoPortMoveMemory((PUCHAR)devExt->GspDmaVirtual + GSP_FW_IMAGE_OFFSET,
                        (PVOID)(_binary_dist_full_gsp_ga104_bin_start + imageOffset),
                        (ULONG)GSP_FW_IMAGE_SIZE);

    /*
     * STAGE 2: Copy authentic GA10x Cryptographic Signature (.fwsignature_ga10x)
     * Size: 4096 bytes (0x1000).
     */
    LogStr("NVX [GSP]: Staging authentic GA10x signature (4KB) at DMA offset 0x");
    LogHex(GSP_SIG_OFFSET);
    LogStr("...\n");
    VideoPortMoveMemory((PUCHAR)devExt->GspDmaVirtual + GSP_SIG_OFFSET,
                        (PVOID)(_binary_dist_full_gsp_ga104_bin_start + signatureOffset),
                        (ULONG)GSP_SIG_SIZE);

    /*
     * STAGE 3: Construct Radix-3 Page Table for GSP Firmware Image.
     * Level 0: 1 4KB page at GSP_RADIX3_LVL0_OFFSET (points to Level 1 page)
     * Level 1: 1 4KB page at GSP_RADIX3_LVL1_OFFSET (19 entries pointing to Level 2 pages)
     * Level 2: 19 4KB pages at GSP_RADIX3_LVL2_OFFSET (9288 entries pointing to .fwimage pages)
     */
    LogStr("NVX [GSP]: Constructing Radix-3 Page Table (21 pages / 84KB)...\n");
    {
        PULONG64 pRadixLvl0 = (PULONG64)((PUCHAR)devExt->GspDmaVirtual + GSP_RADIX3_LVL0_OFFSET);
        PULONG64 pRadixLvl1 = (PULONG64)((PUCHAR)devExt->GspDmaVirtual + GSP_RADIX3_LVL1_OFFSET);
        PULONG64 pRadixLvl2 = (PULONG64)((PUCHAR)devExt->GspDmaVirtual + GSP_RADIX3_LVL2_OFFSET);
        ULONG numPages = (ULONG)((GSP_FW_IMAGE_SIZE + 4095) / 4096); /* 9288 pages */
        ULONG i, k;

        /* Level 0: 1 entry pointing to Level 1 page physical address */
        pRadixLvl0[0] = devExt->GspDmaPhysical.QuadPart + GSP_RADIX3_LVL1_OFFSET;

        /* Level 1: 19 entries, each pointing to a Level 2 page physical address */
        for (i = 0; i < 19; i++) {
            pRadixLvl1[i] = (devExt->GspDmaPhysical.QuadPart + GSP_RADIX3_LVL2_OFFSET) + ((ULONG64)i * 4096ULL);
        }

        /* Level 2: 9288 entries, each pointing to a 4KB page of .fwimage */
        for (k = 0; k < numPages; k++) {
            pRadixLvl2[k] = (devExt->GspDmaPhysical.QuadPart + GSP_FW_IMAGE_OFFSET) + ((ULONG64)k * 4096ULL);
        }

        LogStr("NVX [GSP]: Radix-3 Level 0 at Phys 0x");
        LogHex(devExt->GspDmaPhysical.QuadPart + GSP_RADIX3_LVL0_OFFSET);
        LogStr(", Level 1 at Phys 0x");
        LogHex(devExt->GspDmaPhysical.QuadPart + GSP_RADIX3_LVL1_OFFSET);
        LogStr(", Level 2 at Phys 0x");
        LogHex(devExt->GspDmaPhysical.QuadPart + GSP_RADIX3_LVL2_OFFSET);
        LogStr("\n");
    }

    /* STAGE 4: Stage Bootloader ucode at GSP_BOOTLOADER_OFFSET */
    VideoPortMoveMemory((PUCHAR)devExt->GspDmaVirtual + GSP_BOOTLOADER_OFFSET,
                        (PUCHAR)src_driver_bootloader_535_113_01_bin + blCodeOffset,
                        blCodeSize);

    /* STAGE 5: Stage Booter Load ucode at GSP_BOOTER_LOAD_OFFSET & patch RSA signature */
    VideoPortMoveMemory((PUCHAR)devExt->GspDmaVirtual + GSP_BOOTER_LOAD_OFFSET,
                        (PUCHAR)src_driver_booter_load_535_113_01_bin + btCodeOffset,
                        btCodeSize);
    VideoPortMoveMemory((PUCHAR)devExt->GspDmaVirtual + GSP_BOOTER_LOAD_OFFSET + bt.PatchOffset,
                        src_driver_booter_load_535_113_01_bin + bt.SignatureOffset + signatureIndex * bt.SignatureSize,
                        bt.SignatureSize);

    /*
     * STAGE 6: Authentic GSP firmware WPR metadata V1 in DMA buffer at GSP_WPR_META_OFFSET.
     * This metadata is passed to the SEC2 booter and GSP bootloader.
     */
    PGSP_FW_WPR_META_V1 wprMeta = (PGSP_FW_WPR_META_V1)((PUCHAR)devExt->GspDmaVirtual + GSP_WPR_META_OFFSET);
    PHYSICAL_ADDRESS wprPhys;
    wprPhys.QuadPart = devExt->GspDmaPhysical.QuadPart + GSP_WPR_META_OFFSET;

    VideoPortZeroMemory(wprMeta, sizeof(GSP_FW_WPR_META_V1));
    wprMeta->magic = GSP_FW_WPR_META_MAGIC;
    wprMeta->revision = GSP_FW_WPR_META_REVISION;

    /* SYSMEM pointers for SEC2 to verify and copy */
    wprMeta->sysmemAddrOfRadix3Elf = devExt->GspDmaPhysical.QuadPart + GSP_RADIX3_LVL0_OFFSET;
    wprMeta->sizeOfRadix3Elf = GSP_FW_IMAGE_SIZE;
    wprMeta->sysmemAddrOfSignature = devExt->GspDmaPhysical.QuadPart + GSP_SIG_OFFSET;
    wprMeta->sizeOfSignature = GSP_SIG_SIZE;

    wprMeta->sysmemAddrOfBootloader = devExt->GspDmaPhysical.QuadPart + GSP_BOOTLOADER_OFFSET;
    wprMeta->sizeOfBootloader = blCodeSize;
    wprMeta->bootloaderCodeOffset = blDesc ? blDesc->MonitorCodeOffset : 0x1000;
    wprMeta->bootloaderDataOffset = blDesc ? blDesc->MonitorDataOffset : 0x800;
    wprMeta->bootloaderManifestOffset = blDesc ? blDesc->ManifestOffset : 0x0;

    /* Authentic GA104 (8GB VRAM) Descending Framebuffer Layout */
    wprMeta->fbSize = (ULONG64)VideoPortReadRegisterUlong((PULONG)(bar0+0x1183a4)) << 20;
    if (wprMeta->fbSize < 0x20000000ULL || wprMeta->fbSize > 0x1000000000ULL) {
        LogStr("NVX [GSP]: Invalid hardware VRAM size.\n");
        NvxGspShutdown(HwDeviceExtension);
        return FALSE;
    }
    LogStr("NVX [GSP]: Physical VRAM bytes "); LogHex(wprMeta->fbSize); LogStr("\n");
    wprMeta->vgaWorkspaceOffset = wprMeta->fbSize - 0x100000ULL;
    {
        ULONG workspace = VideoPortReadRegisterUlong((PULONG)(bar0+0x625f04));
        if (workspace & 8) {
            ULONG64 addr = (ULONG64)(workspace & 0xffffff00) << 8;
            wprMeta->vgaWorkspaceOffset = addr < wprMeta->vgaWorkspaceOffset ?
                wprMeta->fbSize - 0x20000 : addr;
        }
        if (wprMeta->vgaWorkspaceOffset >= wprMeta->fbSize) {
            LogStr("NVX [GSP]: Invalid VGA workspace.\n");
            NvxGspShutdown(HwDeviceExtension);
            return FALSE;
        }
    }
    wprMeta->vgaWorkspaceSize = wprMeta->fbSize - wprMeta->vgaWorkspaceOffset;
    wprMeta->gspFwWprEnd = wprMeta->vgaWorkspaceOffset & ~0x1ffffULL;
    wprMeta->frtsSize = 0x100000ULL;
    wprMeta->frtsOffset = wprMeta->gspFwWprEnd - wprMeta->frtsSize;
    if (devExt->FwsecFrtsReady) {
        if (devExt->VerifiedWprEnd < 0x100000ULL ||
            devExt->VerifiedFrtsOffset != devExt->VerifiedWprEnd-0x100000ULL ||
            devExt->VerifiedWprEnd > wprMeta->fbSize ||
            devExt->VerifiedVgaWorkspaceOffset >= wprMeta->fbSize) {
            LogStr("NVX [GSP]: Invalid verified FRTS layout; bootstrap blocked.\n");
            return FALSE;
        }
        wprMeta->vgaWorkspaceOffset=devExt->VerifiedVgaWorkspaceOffset;
        wprMeta->vgaWorkspaceSize=wprMeta->fbSize-wprMeta->vgaWorkspaceOffset;
        wprMeta->gspFwWprEnd=devExt->VerifiedWprEnd;
        wprMeta->frtsOffset=devExt->VerifiedFrtsOffset;
        LogStr("NVX [GSP]: Using verified WPR end ");LogHex(wprMeta->gspFwWprEnd);
        LogStr(" FRTS ");LogHex(wprMeta->frtsOffset);LogStr("\n");
    }
    wprMeta->bootBinOffset = (wprMeta->frtsOffset - wprMeta->sizeOfBootloader) & ~0xFFFULL;
    wprMeta->gspFwOffset = (wprMeta->bootBinOffset - wprMeta->sizeOfRadix3Elf) & ~0xFFFFULL;
    /* 535 LibOS2: 8MB base + 1MB per-GB allowance + 96MB client heap. */
    wprMeta->gspFwHeapSize = 0x6800000ULL +
        ((((wprMeta->fbSize + 0x3fffffffULL) >> 30) * (96ULL << 10) + 0xfffffULL) & ~0xfffffULL);
    wprMeta->gspFwHeapOffset = (wprMeta->gspFwOffset - wprMeta->gspFwHeapSize) & ~0xFFFFFULL;
    wprMeta->gspFwHeapSize = (wprMeta->gspFwOffset-wprMeta->gspFwHeapOffset) & ~0xfffffULL;
    wprMeta->gspFwWprStart = (wprMeta->gspFwHeapOffset-sizeof(*wprMeta)) & ~0xfffffULL;
    wprMeta->nonWprHeapSize = 0x100000ULL;
    wprMeta->nonWprHeapOffset = wprMeta->gspFwWprStart - wprMeta->nonWprHeapSize;
    wprMeta->gspFwRsvdStart = wprMeta->nonWprHeapOffset;
    wprMeta->bootCount = 0;
    wprMeta->verified = 0;

    /* 535 shared memory: PTE page followed by two 256KB queues. */
    {
        ULONG i;
        PULONG64 ptes = (PULONG64)((PUCHAR)devExt->GspDmaVirtual+GSP_SHARED_OFFSET);
        NVX_MSGQ_HEADER *cmd = (NVX_MSGQ_HEADER *)((PUCHAR)devExt->GspDmaVirtual+GSP_CMD_QUEUE_OFFSET);
        PGSP_ARGUMENTS_CACHED args = (PGSP_ARGUMENTS_CACHED)((PUCHAR)devExt->GspDmaVirtual+GSP_RM_ARGS_OFFSET);
        for (i=0;i<129;i++) ptes[i] = devExt->GspDmaPhysical.QuadPart+GSP_SHARED_OFFSET+(ULONG64)i*4096;
        cmd->Size = 0x40000; cmd->MsgSize = 4096; cmd->MsgCount = 63;
        cmd->Flags = 1; cmd->RxHdrOff = 32; cmd->EntryOff = 4096;
        args->MessageQueueInitArguments.SharedMemPhysAddr = devExt->GspDmaPhysical.QuadPart+GSP_SHARED_OFFSET;
        args->MessageQueueInitArguments.PageTableEntryCount = 129;
        args->MessageQueueInitArguments.CmdQueueOffset = 0x1000;
        args->MessageQueueInitArguments.StatQueueOffset = 0x41000;
        devExt->CmdQueueVirtual = cmd;
        devExt->StatQueueVirtual = (PUCHAR)devExt->GspDmaVirtual+GSP_STAT_QUEUE_OFFSET;
        devExt->CmdQueueSize = devExt->StatQueueSize = 0x40000;
        {
            NVX_SYSTEM_INFO_535 sys;
            ULONG registry[2]={8,0};
            PUCHAR entries=(PUCHAR)cmd+4096;
            VideoPortZeroMemory(&sys,sizeof(sys));
            sys.gpuPhysAddr=devExt->PhysicalBar0.QuadPart;
            sys.gpuPhysFbAddr=devExt->PhysicalBar1.QuadPart;
            sys.gpuPhysInstAddr=devExt->PhysicalInstBar.QuadPart;
            sys.dbdf=((ULONG64)devExt->PciBusNumber<<8)|(devExt->PciSlot&31U);
            /* Conservative user VA ceiling until OS capabilities are plumbed. */
            sys.maxUserVa=0x7ffeffffULL;
            sys.mirrorBase=0x88000;sys.mirrorSize=0x1000;
            sys.hypervisor=3;sys.passthrough=TRUE;
            if (!NvxEncodeRpc(entries,4096,72,0,&sys,sizeof(sys)) ||
                !NvxEncodeRpc(entries+4096,4096,73,1,registry,sizeof(registry))) {
                LogStr("NVX [RPC]: startup packet preparation failed.\n");return FALSE;
            }
            LogStr("NVX [RPC]: prepared SYSTEM_INFO bytes=");LogHex(sizeof(sys));
            LogStr(" BAR0=");LogHex(sys.gpuPhysAddr);LogStr(" BAR1=");LogHex(sys.gpuPhysFbAddr);
            LogStr(" INST=");LogHex(sys.gpuPhysInstAddr);LogStr(" DBDF=");LogHex(sys.dbdf);
            LogStr("; empty REGISTRY prepared.\n");
            /* Publish both complete slots before Booter can start RM. */
            __sync_synchronize();
            cmd->WritePtr=2;
            __sync_synchronize();
            LogStr("NVX [RPC]: published startup SYSTEM_INFO then REGISTRY; write=2.\n");
        }
    }
    PLIBOS_MEMORY_REGION_INIT_ARGUMENT libosArgs = (PLIBOS_MEMORY_REGION_INIT_ARGUMENT)((PUCHAR)devExt->GspDmaVirtual+GSP_LIBOS_ARGS_OFFSET);
    PHYSICAL_ADDRESS libosPhys;
    libosPhys.QuadPart = devExt->GspDmaPhysical.QuadPart+GSP_LIBOS_ARGS_OFFSET;
    {
        static const ULONG64 ids[] = {0x4c4f47494e4954ULL, 0x4c4f47494e5452ULL, 0x4c4f47524dULL, 0x524d41524753ULL};
        ULONG i,j;
        for (i=0;i<4;i++) {
            ULONG64 off = i==3 ? GSP_RM_ARGS_OFFSET : 0x2581000ULL+(ULONG64)i*0x10000;
            libosArgs[i].Id8 = ids[i];
            libosArgs[i].Pa = devExt->GspDmaPhysical.QuadPart+off;
            libosArgs[i].Size = i==3 ? 4096 : 65536;
            libosArgs[i].Kind = libosArgs[i].Loc = 1;
            if (i<3) {
                PULONG64 log = (PULONG64)((PUCHAR)devExt->GspDmaVirtual+off);
                for (j=0;j<16;j++) log[j+1] = libosArgs[i].Pa+(ULONG64)j*4096;
            }
        }
    }

    LogStr("NVX [GSP]: Staging DMA buffer at Virtual 0x");
    LogHex((unsigned long long)devExt->GspDmaVirtual);
    LogStr(", Physical 0x");
    LogHex(devExt->GspDmaPhysical.QuadPart);
    LogStr("\n");
    LogStr("NVX [GSP]: WPR Metadata at Physical 0x");
    LogHex(wprPhys.QuadPart);
    LogStr("\n");

    {
        ULONG boot0 = VideoPortReadRegisterUlong((PULONG)bar0);
        ULONG flcnStat = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x1100F4));
        ULONG sec2Stat = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x8400F4));
        ULONG dispOwner = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x6254E8));
        ULONG dispCore = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x6104E0));
        ULONG dispPut = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x680000));
        ULONG dispDma = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x610B24));
        ULONG headRg = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x616300));
        LogStr("NVX [GSP]: BAR0[0]: 0x"); LogHex(boot0);
        LogStr(" Flcn 0x1100F4: 0x"); LogHex(flcnStat);
        LogStr(" SEC2 0x8400F4: 0x"); LogHex(sec2Stat);
        LogStr("\n");
        LogStr("NVX [DISP]: Owner(0x6254E8): 0x"); LogHex(dispOwner);
        LogStr(" Core(0x6104E0): 0x"); LogHex(dispCore);
        LogStr(" Put(0x680000): 0x"); LogHex(dispPut);
        LogStr(" Dma(0x610B24): 0x"); LogHex(dispDma);
        LogStr(" HeadCapability(0x616300): 0x"); LogHex(headRg);
        LogStr("\n");
    }

    /*
     * STEP 1: Reset GSP Falcon, enable RISC-V mode, and program boot arguments.
     * In GA104, GSP RISC-V is unlocked and booted by the SEC2 booter load ucode.
     */
    LogStr("NVX [GSP]: Preparing GSP Falcon for RISC-V execution...\n");

    /* Wait for Falcon reset prep */
    timeout = 1000;
    while (!(VideoPortReadRegisterUlong((PULONG)(bar0 + 0x1100F4)) & 0x80000000) && --timeout) {
        VideoPortStallExecution(10);
    }

    /* Reset GSP Falcon engine */
    VideoPortWriteRegisterUlong((PULONG)(bar0 + NV_PGSP_FALCON_ENGINE), 1);
    VideoPortStallExecution(20);
    VideoPortWriteRegisterUlong((PULONG)(bar0 + NV_PGSP_FALCON_ENGINE), 0);
    VideoPortStallExecution(20);

    /* Wait for GSP memory scrubbing to finish */
    timeout = 2000;
    while ((VideoPortReadRegisterUlong((PULONG)(bar0 + 0x1100F4)) & 0x00001000) && --timeout) {
        VideoPortStallExecution(10);
    }
    if (timeout == 0) {
        LogStr("NVX [GSP]: GSP memory scrubbing wait timeout!\n");
        return FALSE;
    } else {
        LogStr("NVX [GSP]: GSP memory scrubbing complete!\n");
    }

    /* Put GSP Falcon into RISC-V mode (0x111668 |= 0x111) */
    {
        ULONG bcr = VideoPortReadRegisterUlong((PULONG)(bar0 + NV_PGSP_FALCON_BCR));
        VideoPortWriteRegisterUlong((PULONG)(bar0 + NV_PGSP_FALCON_BCR), bcr | 0x111);
    }

    /* Pass Libos init arguments physical base to GSP Mailbox 0 / 1 */
    VideoPortWriteRegisterUlong((PULONG)(bar0 + NV_PGSP_FALCON_MAILBOX0),
                               (ULONG)(libosPhys.QuadPart & 0xFFFFFFFF));
    VideoPortWriteRegisterUlong((PULONG)(bar0 + NV_PGSP_FALCON_MAILBOX1),
                               (ULONG)(libosPhys.QuadPart >> 32));

    /*
     * STEP 2: Execute Booter Load on SEC2 Falcon (0x840000).
     * SEC2 verifies the WPR metadata, initializes WPR in VRAM, loads the bootloader into GSP, and unlocks GSP.
     */
    LogStr("NVX [SEC2]: Preparing SEC2 Falcon to execute Booter Load...\n");

    NvxLogSec2State(bar0, "before reset");
    if (!NvxSec2ResetEnable(bar0)) {
        LogStr("NVX [SEC2]: Reset/enable sequence failed.\n");
        return FALSE;
    }
    NvxLogSec2State(bar0, "after reset");

    /* Setup SEC2 DMA interface */
    {
        ULONG val = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x840624));
        VideoPortWriteRegisterUlong((PULONG)(bar0 + 0x840624), val | 0x80);
        VideoPortWriteRegisterUlong((PULONG)(bar0 + 0x84010C), 0);
        val = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x840600));
        VideoPortWriteRegisterUlong((PULONG)(bar0 + 0x840600), (val & ~0x00010007) | 5);
    }

    ULONG64 booterPhys = devExt->GspDmaPhysical.QuadPart+GSP_BOOTER_LOAD_OFFSET;
    if (!NvxSec2Dma(bar0,booterPhys,bt.ImemOffset,bt.ImemSize,TRUE) ||
        !NvxSec2Dma(bar0,booterPhys,bt.DmemOffset,bt.DmemSize,FALSE)) return FALSE;

    VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON2_PARAADDR),bt.PatchOffset-bt.DmemOffset);
    VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON2_ENGIDMASK),bt.EngineId);
    VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON2_UCODE_ID),bt.UcodeId);
    VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON2_MOD_SEL),1);

    /* Provide WPR metadata address in SEC2 mailboxes */
    VideoPortWriteRegisterUlong((PULONG)(bar0 + NV_PSEC_FALCON_MAILBOX0), (ULONG)(wprPhys.QuadPart & 0xFFFFFFFF));
    VideoPortWriteRegisterUlong((PULONG)(bar0 + NV_PSEC_FALCON_MAILBOX1), (ULONG)(wprPhys.QuadPart >> 32));

    /* Check CPUCTL and start CPU.  Turing+ Falcons require the alias register
       when CPUCTL.ALIAS_EN is set; this is the exact OpenRM 535 behavior. */
    ULONG cpuCtl = VideoPortReadRegisterUlong((PULONG)(bar0 + NV_PSEC_FALCON_CPUCTL));
    LogStr("NVX [SEC2]: CPUCTL before start: 0x"); LogHex(cpuCtl); LogStr("\n");

    VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_BOOTVEC),bt.ImemOffset);
    if (cpuCtl & 0x40) {
        LogStr("NVX [SEC2]: Starting through CPUCTL_ALIAS.\n");
        VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_CPUCTL_ALIAS),2);
    } else {
        LogStr("NVX [SEC2]: Starting through CPUCTL.\n");
        VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_CPUCTL),2);
    }
    NvxLogSec2State(bar0, "immediately after start");
    /* Boot ROM authentication may finish before we observe RUNNING. Require
       HALTED + zero status + verified metadata, rather than seeing an edge. */
    {
        BOOLEAN sawRunning = FALSE;
        ULONG lastCtl = cpuCtl;
        timeout = 100000;
        while (timeout) {
            ULONG ctl = VideoPortReadRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_CPUCTL));
            ULONG status = VideoPortReadRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_MAILBOX0));
            ULONG64 verified = ((volatile GspFwWprMeta *)wprMeta)->verified;
            if (ctl != lastCtl) {
                LogStr("NVX [SEC2]: CPUCTL transition: 0x"); LogHex(lastCtl);
                LogStr(" -> 0x"); LogHex(ctl); LogStr("\n");
                lastCtl = ctl;
            }
            if (ctl != 0xffffffff) {
                if (!(ctl & 0x10)) sawRunning = TRUE;
                if ((ctl & 0x10) && (sawRunning || status != (ULONG)wprPhys.QuadPart ||
                                     verified == GSP_FW_WPR_META_VERIFIED)) break;
            }
            timeout--;
            VideoPortStallExecution(50);
        }
        if (!timeout) {
            LogStr("NVX [SEC2]: Execution timeout; stale HALTED is not completion.\n");
            return FALSE;
        }
    }
    {
        ULONG status = VideoPortReadRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_MAILBOX0));
        ULONG status1 = VideoPortReadRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_MAILBOX1));
        ULONG64 verified = ((volatile GspFwWprMeta *)wprMeta)->verified;
        LogStr("NVX [SEC2]: Halted. Mailbox0: "); LogHex(status);
        LogStr(" Mailbox1: "); LogHex(status1);
        LogStr(" Verified: "); LogHex(verified);
        LogStr(" SEC2 CPUCTL: "); LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_CPUCTL)));
        LogStr("\n");
        /* Booter authenticates/copies metadata into protected framebuffer.
           The input sysmem copy need not receive the Verified writeback.
           OpenRM accepts a completed Booter with mailbox0 == 0. */
        if (status) {
            LogStr("NVX [SEC2]: Booter failed; RM startup skipped.\n");
            return FALSE;
        }
        devExt->GspBootVerified = TRUE;
    }

    /*
     * STEP 3: Setup GSP Falcon OS version and verify RISC-V execution.
     */
    VideoPortWriteRegisterUlong((PULONG)(bar0 + NV_PGSP_FALCON_OS), blDesc ? blDesc->AppVersion : 0);

    {
        ULONG riscvStat = VideoPortReadRegisterUlong((PULONG)(bar0 + NV_PGSP_FALCON_RISCV_STAT));
        ULONG bcrVal = VideoPortReadRegisterUlong((PULONG)(bar0 + NV_PGSP_FALCON_BCR));
        LogStr("NVX [GSP]: RISC-V Active (0x111388): 0x"); LogHex(riscvStat);
        LogStr(" BCR (0x111668): 0x"); LogHex(bcrVal);
        LogStr("\n");
    }

    /* No 0x55aa55aa mailbox handshake exists in RM 535. RISC-V activity
       and WPR verification do not establish RM readiness. Poll the real queue. */
    LogStr("NVX [GSP]: Waiting for RM INIT_DONE event (0x1001).\n");
    ULONG rxSequence=0;
    timeout = 20000;
    while (timeout--) {
        volatile NVX_MSGQ_HEADER *msg = (volatile NVX_MSGQ_HEADER *)devExt->StatQueueVirtual;
        volatile NVX_MSGQ_HEADER *local=(volatile NVX_MSGQ_HEADER *)devExt->CmdQueueVirtual;
        ULONG write=msg->WritePtr,read=local->ReadPtr;
        if (write>=63||read>=63) {
            LogStr("NVX [RPC]: invalid queue pointer; receive stopped.\n");return FALSE;
        }
        if (write!=read) {
            uint32_t function,result,sequence,elements=0,decodedElements=0;
            const uint8_t *entry=devExt->RpcRxRecord;
            int gathered;
            BOOLEAN consumed=FALSE;
            __sync_synchronize();
            gathered=NvxGatherRpc((const uint8_t *)devExt->StatQueueVirtual+4096,
                63*4096,63,read,write,devExt->RpcRxRecord,sizeof(devExt->RpcRxRecord),&elements);
            if (!gathered) { VideoPortStallExecution(100);continue; }
            if (gathered<0) {
                LogStr("NVX [RPC]: invalid record element count; receive stopped.\n");return FALSE;
            }
            if (msg->MsgSize!=4096||msg->MsgCount!=63||msg->EntryOff!=4096||
                !NvxDecodeRpcRecord(entry,elements*4096,&function,&result,&sequence,&decodedElements)||
                decodedElements!=elements||sequence!=rxSequence) {
                const uint32_t *raw=(const uint32_t *)entry;
                LogStr("NVX [RPC]: invalid checksum/header/sequence; read=");LogHex(read);
                LogStr(" write=");LogHex(write);LogStr(" expected_seq=");LogHex(rxSequence);
                LogStr(" seq=");LogHex(raw[9]);LogStr(" elements=");LogHex(raw[10]);
                LogStr(" version=");LogHex(raw[12]);LogStr(" signature=");LogHex(raw[13]);
                LogStr(" length=");LogHex(raw[14]);LogStr(" function=");LogHex(raw[15]);
                LogStr(" result=");LogHex(raw[16]);LogStr("; receive stopped.\n");return FALSE;
            }
            /* Avoid serial output before acknowledging the sequencer. */
            if(function!=0x1002||result) {
                LogStr("NVX [RPC]: startup event=");LogHex(function);
                LogStr(" result=");LogHex(result);LogStr(" sequence=");LogHex(sequence);LogStr("\n");
            }
            if (function==0x1002 && !result) {
                const uint32_t *words=(const uint32_t *)entry;
                const uint32_t *params=words+20;
                uint32_t count=0;
                if (!NvxValidateSeq(params,words[14]-32,devExt->Bar0Length,&count)) {
                    LogStr("NVX [SEQ]: rejected malformed sequencer request.\n");return FALSE;
                }
                /* Match OpenRM receive ordering: acknowledge the validated
                   copy before dispatch or slow serial diagnostics. */
                __sync_synchronize();local->ReadPtr=(read+elements)%63;__sync_synchronize();
                rxSequence++;consumed=TRUE;
                LogStr("NVX [SEQ]: validated capacity=");LogHex(params[0]);
                LogStr(" used=");LogHex(params[1]);LogStr(" commands=");LogHex(count);LogStr("\n");
                {
                    NVX_SEQ_IO io={devExt,NvxSeqRead,NvxSeqWrite,NvxSeqDelay,NvxGspSequencerCore};
                    uint32_t failedWord=0;
                    if(!NvxExecuteSeq((uint32_t *)params,words[14]-32,devExt->Bar0Length,&io,&failedWord)) {
                        LogStr("NVX [SEQ]: execution failed at word=");LogHex(failedWord);
                        if(failedWord<params[1]) {
                            LogStr(" opcode=");LogHex(params[10+failedWord]);
                            if(params[10+failedWord]==2) {
                                ULONG reg=params[11+failedWord];
                                LogStr(" reg=");LogHex(reg);
                                LogStr(" value=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+reg)));
                                LogStr(" mask=");LogHex(params[12+failedWord]);
                                LogStr(" expected=");LogHex(params[13+failedWord]);
                            }
                        }
                        LogStr("; staging retained.\n");return FALSE;
                    }
                    LogStr("NVX [SEQ]: execution completed.\n");
                }
            }
            if (function==0x100c && !result) {
                /* RM 535 rpc_ucode_libos_print_v1E_08: two U32s then bytes.
                   Diagnostic notification, no reply or register operations. */
                const uint32_t *words=(const uint32_t *)entry;
                uint32_t bytes=words[14]-32;
                if(bytes<8 || words[21]>bytes-8) {
                    LogStr("NVX [LIBOS]: malformed print payload retained.\n");return FALSE;
                }
                LogStr("NVX [LIBOS]: engine=");LogHex(words[20]);
                LogStr(" bytes=");LogHex(words[21]);LogStr("\n");
                for(uint32_t i=0;i<words[21] && i<256;i+=4) {
                    uint32_t value=0;
                    for(uint32_t j=0;j<4 && i+j<words[21];j++)
                        value|=(uint32_t)entry[88+i+j]<<(8*j);
                    LogStr("NVX [LIBOS-WORD]: offset=");LogHex(i);
                    LogStr(" value=");LogHex(value);LogStr("\n");
                }
            }
            if (result || (function!=72 && function!=73 && function!=0x1001 && function!=0x101b && function!=0x1002 && function!=0x100c)) {
                LogStr("NVX [RPC]: unhandled startup event retained for diagnosis.\n");return FALSE;
            }
            if (function==0x101b) {
                /* 535 GspUserSharedData contains gspAssertCount. */
                const uint32_t *words=(const uint32_t *)entry;
                if (words[14]<36) return FALSE;
                devExt->GspAssertCount=words[20];
                LogStr("NVX [RPC]: firmware assertion count=");
                LogHex(devExt->GspAssertCount);LogStr("\n");
            }
            if(!consumed) {
                __sync_synchronize();local->ReadPtr=(read+elements)%63;__sync_synchronize();
                rxSequence++;
            }
            if (function==0x1001) {
                if (!(VideoPortReadRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_RISCV_STAT))&0x80))
                    return FALSE;
                devExt->GspInitialized=TRUE;
                LogStr("NVX [GSP]: RM INIT_DONE received; display channels still unavailable.\n");
                if(!NvxGspCreateObjects(devExt,rxSequence))
                    LogStr("NVX [RM]: acceleration bring-up incomplete; software display retained.\n");
                return TRUE;
            }
            continue;
        }
        VideoPortStallExecution(100);
    }
    LogStr("NVX [GSP]: RM INIT_DONE timeout; capturing bounded startup diagnostics.\n");
    {
        volatile ULONG *queue=(volatile ULONG *)devExt->StatQueueVirtual;
        ULONG i,j;
        LogStr("NVX [RM-DIAG]: status queue header");
        for (i=0;i<9;i++) { LogStr(" ");LogHex(queue[i]); }
        LogStr("\n");
        LogStr("NVX [RM-DIAG]: host command write=");
        LogHex(((volatile NVX_MSGQ_HEADER *)devExt->CmdQueueVirtual)->WritePtr);
        LogStr(" firmware command read=");LogHex(((volatile NVX_MSGQ_HEADER *)devExt->StatQueueVirtual)->ReadPtr);
        LogStr("\n");
        for (i=0;i<3;i++) {
            volatile UCHAR *log=(volatile UCHAR *)devExt->GspDmaVirtual+
                0x2581000ULL+(ULONG64)i*0x10000;
            ULONG nonzero=0,printed=0;
            LogStr("NVX [RM-DIAG]: log index=");LogHex(i);
            LogStr(" header");
            for (j=0;j<8;j++) { LogStr(" ");LogHex(((volatile ULONG64 *)log)[j]); }
            LogStr("\n");
            /* 535 LibOS PUT counts 64-bit ring entries, excluding PUT itself.
               Preserve the latest 512 words exactly for offline decoding. */
            {
                volatile ULONG64 *words=(volatile ULONG64 *)log;
                ULONG64 put=words[0],first=put>512 ? put-512 : 0,k;
                LogStr("NVX [RM-RING]: log=");LogHex(i);
                LogStr(" put=");LogHex(put);LogStr(" first=");LogHex(first);LogStr("\n");
                for (k=first;k<put;k++) {
                    LogStr("NVX [RM-WORD]: log=");LogHex(i);
                    LogStr(" index=");LogHex(k);
                    LogStr(" value=");LogHex(words[1+(k%8191)]);LogStr("\n");
                }
            }
            for (j=128;j<65536;j++) if (log[j]) nonzero++;
            (void)printed;
            LogStr("\nNVX [RM-DIAG]: nonzero_bytes=");LogHex(nonzero);LogStr("\n");
        }
        LogStr("NVX [RM-DIAG]: riscv=");
        LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_RISCV_STAT)));
        LogStr(" mailbox0=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_MAILBOX0)));
        LogStr(" mailbox1=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_MAILBOX1)));
        LogStr("\n");
    }
    return FALSE;
}

VOID NvxGspShutdown(PVOID HwDeviceExtension) {
    PNVX_DEVICE_EXTENSION devExt = (PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    PUCHAR bar0 = (PUCHAR)devExt->Bar0MmioVirtual;

    if (devExt->GspDmaVirtual) {
        /* Resetting GSP/SEC2 alone does NOT stop a scheduled CE/PBDMA.
           Tear down the RM client and its channels before freeing their
           backing pages. If RM cannot confirm teardown, retain the DMA. */
        if(devExt->GspInitialized&&devExt->GspClientHandle) {
            ULONG release[4]={devExt->GspClientHandle,0,devExt->GspClientHandle,0};
            const ULONG *reply=(const ULONG *)devExt->RpcRxRecord;
            if(!NvxGspCall(devExt,10,release,sizeof(release))||reply[14]<48||
               reply[20]!=release[0]||reply[21]!=0||reply[22]!=release[2]||reply[23]!=0) {
                LogStr("NVX [GSP]: RM teardown unconfirmed; DMA backing retained for channel safety.\n");
                return;
            }
            devExt->GspClientHandle=0;
            LogStr("NVX [GSP]: RM client and channels released before DMA teardown.\n");
        }
        if (bar0) {
            VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_ENGINE),1);
            VideoPortWriteRegisterUlong((PULONG)(bar0 + NV_PGSP_FALCON_ENGINE), 1); /* Halt Falcon */
            /* Flush posted resets before releasing DMA-visible storage. */
            (void)VideoPortReadRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_ENGINE));
            (void)VideoPortReadRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_ENGINE));
            VideoPortStallExecution(20);
        }

        /* The GR backing can be GPU-visible; release it only after the same
           confirmed RM teardown and engine halt as the GSP staging buffer. */
        if(devExt->GspGrContextVirtual) {
            VideoPortFreeCommonBuffer(HwDeviceExtension,devExt->GspGrContextLength,
                devExt->GspGrContextVirtual,devExt->GspGrContextPhysical,FALSE);
            devExt->GspGrContextVirtual=NULL;
            devExt->GspGrContextLength=0;
            devExt->GspGrContextMappingVerified=FALSE;
        }
        VideoPortFreeCommonBuffer(
            HwDeviceExtension,
            devExt->GspDmaLength,
            devExt->GspDmaVirtual,
            devExt->GspDmaPhysical,
            TRUE
        );

        devExt->GspDmaVirtual = NULL;
        devExt->CmdQueueVirtual = devExt->StatQueueVirtual = NULL;
        devExt->GspInitialized = FALSE;
        devExt->GspBootVerified = FALSE;
        devExt->GspCopyVerified = FALSE;
        devExt->GspChannelFaulted = FALSE;
        devExt->GspFbLayoutValid = FALSE;
        devExt->GspScanoutMapped = FALSE;

        if (devExt->DmaAdapter) {
            VideoPortPutDmaAdapter(HwDeviceExtension, devExt->DmaAdapter);
            devExt->DmaAdapter = NULL;
        }

        LogStr("NVX [GSP]: Subsystem halted and buffers released.\n");
        VideoPortDebugPrint(Info, "NVX [GSP]: Subsystem halted and buffers released.\n");
    }
}

