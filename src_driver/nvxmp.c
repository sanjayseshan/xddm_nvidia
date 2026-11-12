/*
 * NVIDIA GeForce RTX 3070 (Ampere GA104) XDDM Video Miniport Driver
 * Target: Windows XP Professional x64 Edition / Server 2003 x64 (NT 5.2)
 *
 * Driver Version: 3.0.0.5
 */

#include "miniport.h"
#include "nvx_gsp.h"
#include "nvx_vbe.h"
#include "nvx_vbios.h"

#define NV_VENDOR_ID 0x10DE

/* Compare the video-port callback's process context with the user-mode NVX
 * client token during XP channel bring-up diagnostics. */
extern HANDLE NTAPI PsGetCurrentProcessId(VOID);
extern ULONG NTAPI RtlRandomEx(PULONG Seed);

/* NT kernel file APIs used by the optional boot diagnostic log. */
typedef struct _NVX_OBJECT_ATTRIBUTES {
    ULONG Length;
    HANDLE RootDirectory;
    struct _NVX_UNICODE_STRING *ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} NVX_OBJECT_ATTRIBUTES, *PNVX_OBJECT_ATTRIBUTES;

typedef struct _NVX_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} NVX_UNICODE_STRING;

typedef struct _NVX_IO_STATUS_BLOCK {
    union { LONG Status; PVOID Pointer; } u;
    ULONG_PTR Information;
} NVX_IO_STATUS_BLOCK, *PNVX_IO_STATUS_BLOCK;

extern LONG NTAPI ZwCreateFile(PHANDLE, ACCESS_MASK, PNVX_OBJECT_ATTRIBUTES,
    PNVX_IO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
extern LONG NTAPI ZwWriteFile(HANDLE, HANDLE, PVOID, PVOID, PNVX_IO_STATUS_BLOCK,
    PVOID, ULONG, PLARGE_INTEGER, PULONG);
#if defined(__i386__)
static inline UCHAR NTAPI KeGetCurrentIrql(VOID) {
    UCHAR irql;
    __asm__ __volatile__("movb %%fs:0x24, %0" : "=q"(irql));
    return irql;
}
#else
extern UCHAR NTAPI KeGetCurrentIrql(VOID);
#endif

#ifndef __VXDKERNEL__
#define NVX_LOG_RING_BYTES 1048576UL
#define NVX_LOG_FLUSH_CHUNK 1024UL
#define NVX_LOG_PASSIVE_LEVEL 0
static HANDLE g_LogFile;
static UCHAR g_LogRing[NVX_LOG_RING_BYTES];
static volatile ULONG64 g_LogWriteSequence;
static ULONG64 g_LogReadSequence;
static ULONG g_LogOpenRetry;
static BOOLEAN g_LogMarkerWritten;
static volatile LONG g_LogFlushBusy;

static VOID NvxLogAppend(const CHAR *text, ULONG length)
{
    ULONG i;
    for (i = 0; i < length; ++i) {
        ULONG64 sequence=__sync_fetch_and_add(&g_LogWriteSequence,1);
        g_LogRing[sequence % NVX_LOG_RING_BYTES]=(UCHAR)text[i];
    }
}

/* File I/O is only attempted from PASSIVE_LEVEL. LogStr itself is also used
 * by callbacks that may run at elevated IRQL, so every message first enters
 * the resident ring and disk flushing is opportunistic. */
static VOID NvxLogFlush(VOID)
{
    static WCHAR pathBuffer[] = L"\\??\\C:\\WINDOWS\\nvxmp.log";
    static const CHAR marker[] = "\r\n===== NVX diagnostic run =====\r\n";
    static const CHAR overflowPrefix[] = "NVX [LOG]: ring overflow; dropped bytes=";
    NVX_UNICODE_STRING path;
    NVX_OBJECT_ATTRIBUTES attributes;
    NVX_IO_STATUS_BLOCK io;
    LARGE_INTEGER append;
    CHAR chunk[NVX_LOG_FLUSH_CHUNK];
    ULONG count, dropped, i;
    ULONG64 end;
    LONG status;

    if (KeGetCurrentIrql() != NVX_LOG_PASSIVE_LEVEL ||
        __sync_lock_test_and_set(&g_LogFlushBusy, 1))
        return;

    if (!g_LogFile) {
        if (g_LogOpenRetry) {
            --g_LogOpenRetry;
            __sync_lock_release(&g_LogFlushBusy);
            return;
        }
        path.Buffer = pathBuffer;
        path.Length = (USHORT)(sizeof(pathBuffer) - sizeof(WCHAR));
        path.MaximumLength = sizeof(pathBuffer);
        attributes.Length = sizeof(attributes);
        attributes.RootDirectory = NULL;
        attributes.ObjectName = &path;
        attributes.Attributes = 0x40; /* OBJ_CASE_INSENSITIVE */
        attributes.SecurityDescriptor = NULL;
        attributes.SecurityQualityOfService = NULL;
        status = ZwCreateFile(&g_LogFile, 0x00100004, &attributes, &io, NULL,
            0x80, 0x00000007, 0x00000003, 0x00000020, NULL, 0);
        if (status < 0) {
            g_LogFile = NULL;
            g_LogOpenRetry = 31; /* retry after 32 safe flush opportunities */
            __sync_lock_release(&g_LogFlushBusy);
            return;
        }
    }

    append.QuadPart = -1;
    if (!g_LogMarkerWritten) {
        status = ZwWriteFile(g_LogFile, NULL, NULL, NULL, &io, (PVOID)marker,
            sizeof(marker) - 1, &append, NULL);
        if (status < 0) {
            __sync_lock_release(&g_LogFlushBusy);
            return;
        }
        g_LogMarkerWritten = TRUE;
    }

    end=__sync_fetch_and_add(&g_LogWriteSequence,0);
    dropped=0;
    if (end-g_LogReadSequence > NVX_LOG_RING_BYTES) {
        dropped=(ULONG)((end-g_LogReadSequence)-NVX_LOG_RING_BYTES);
        g_LogReadSequence=end-NVX_LOG_RING_BYTES;
    }
    if (dropped) {
        CHAR line[sizeof(overflowPrefix) + 20];
        ULONG at = sizeof(overflowPrefix) - 1;
        for (i = 0; i < at; ++i) line[i] = overflowPrefix[i];
        line[at++] = '0'; line[at++] = 'x';
        for (i = 0; i < 16; ++i) {
            ULONG shift = (15 - i) * 4;
            ULONG nibble = (dropped >> shift) & 0xFUL;
            line[at++] = (CHAR)(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
        }
        line[at++] = '\r'; line[at++] = '\n';
        status = ZwWriteFile(g_LogFile, NULL, NULL, NULL, &io, line, at,
            &append, NULL);
        if (status < 0) {
            __sync_lock_release(&g_LogFlushBusy);
            return;
        }
    }

    for (;;) {
        end=__sync_fetch_and_add(&g_LogWriteSequence,0);
        count=(ULONG)(end-g_LogReadSequence);
        if (count > sizeof(chunk)) count = sizeof(chunk);
        for (i = 0; i < count; ++i)
            chunk[i] = (CHAR)g_LogRing[(g_LogReadSequence + i) % NVX_LOG_RING_BYTES];
        if (!count) break;

        status = ZwWriteFile(g_LogFile, NULL, NULL, NULL, &io, chunk, count,
            &append, NULL);
        if (status < 0 || io.Information != count) break;

        g_LogReadSequence+=count;
    }
    __sync_lock_release(&g_LogFlushBusy);
}
#else
static VOID NvxLogAppend(const CHAR *text, ULONG length) { (void)text; (void)length; }
static VOID NvxLogFlush(VOID) {}
#endif

/* COM1 Direct Serial Logging (I/O Port 0x3F8) for QEMU VM */
static inline unsigned char inb(unsigned short port) {
    unsigned char val;
    __asm__ __volatile__("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outb(unsigned short port, unsigned char val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void NvxLogSerial(const char *s, ULONG length) {
    ULONG i;
    for (i = 0; i < length; ++i) {
        if (s[i] == '\n') {
            for (volatile ULONG to = 0; (inb(0x3FD) & 0x20) == 0 && to < 100000; to++);
            outb(0x3F8, '\r');
        }
        for (volatile ULONG to = 0; (inb(0x3FD) & 0x20) == 0 && to < 100000; to++);
        outb(0x3F8, (unsigned char)s[i]);
    }
}

void LogStr(const char *s) {
    ULONG length = (ULONG)__builtin_strlen(s);
    NvxLogSerial(s, length);
    NvxLogAppend(s, length);
    /* VideoPortDebugPrint remains available for debugger capture. */
    VideoPortDebugPrint(Info, "%s", s);
    if (length && s[length - 1] == '\n') NvxLogFlush();
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

VOID NvxProbeFwsecRom(PVOID hwDeviceExtension,PUCHAR bar0)
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

BOOLEAN NTAPI HwVidInterrupt(PVOID HwDeviceExtension) {
    (void)HwDeviceExtension;
    /* RM queue interrupts need their own dispatcher/acknowledgement path. */
    return FALSE;
}

BOOLEAN NTAPI HwVidResetHw(PVOID HwDeviceExtension, ULONG Columns, ULONG Rows) {
    (void)HwDeviceExtension;
    (void)Columns;
    (void)Rows;
    /* VideoPort can call this at bugcheck/high IRQL.  Do not stall, reset a
       Falcon, release common buffers, or destroy a DMA adapter here. */
    LogStr("NVX [RESET]: HwVidResetHw invoked; allocations left intact.\n");
    return TRUE;
}

VP_STATUS NTAPI HwVidSetPowerState(
    PVOID HwDeviceExtension,
    ULONG HwId,
    PVIDEO_POWER_MANAGEMENT VideoPowerControl
) {
    (void)HwDeviceExtension;
    (void)HwId;
    (void)VideoPowerControl;
    return NO_ERROR;
}

VP_STATUS NTAPI HwVidGetPowerState(
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

VP_STATUS NTAPI HwVidGetVideoChildDescriptor(
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
    LogHex(ChildEnumInfo->ChildIndex);
    LogStr(" IsSecondary: ");
    LogHex(dev->IsSecondaryDisplay);
    LogStr(" MonitorCount: ");
    LogHex(dev->MonitorCount);
    LogStr("\n");
    if(ChildEnumInfo->ChildIndex==0xffffffffUL) {
        *VideoChildType=VideoChip;*UId=0xffffffffUL;
        return VIDEO_ENUM_MORE_DEVICES;
    }

    if (dev->IsSecondaryDisplay) {
        /* Secondary adapter enumerates the second physical monitor (display 1) */
        PNVX_DEVICE_EXTENSION prim = (PNVX_DEVICE_EXTENSION)dev->PrimaryDeviceExtension;
        if (!prim || prim->MonitorCount < 2)
            return VIDEO_ENUM_NO_MORE_DEVICES;

        if (ChildEnumInfo->ChildIndex == 0) {
            if (ChildEnumInfo->ACPIHwId != prim->MonitorIds[1] &&
                ChildEnumInfo->ACPIHwId != 2)
                return VIDEO_ENUM_INVALID_DEVICE;
        } else if (ChildEnumInfo->ChildIndex != 1) {
            return VIDEO_ENUM_NO_MORE_DEVICES;
        }

        if (!pChildDescriptor || ChildEnumInfo->ChildDescriptorSize < 128)
            return VIDEO_ENUM_INVALID_DEVICE;

        VideoPortMoveMemory(pChildDescriptor, prim->MonitorEdid[1], 128);
        *VideoChildType = Monitor;
        *UId = prim->MonitorIds[1];
        LogStr("NVX [DUALVIEW]: Secondary enumerate monitor id=");
        LogHex(*UId);
        LogStr("\n");
        return VIDEO_ENUM_MORE_DEVICES;
    }

    /* Primary adapter enumerates attached primary monitor only (DualView architecture) */
    if (ChildEnumInfo->ChildIndex == 0) {
        if (dev->MonitorCount == 0) return VIDEO_ENUM_INVALID_DEVICE;
        if (ChildEnumInfo->ACPIHwId != dev->MonitorIds[0] &&
            ChildEnumInfo->ACPIHwId != 1)
            return VIDEO_ENUM_INVALID_DEVICE;
        index = 0;
    } else if (ChildEnumInfo->ChildIndex == 1) {
        if (dev->MonitorCount == 0) return VIDEO_ENUM_NO_MORE_DEVICES;
        index = 0;
    } else {
        return VIDEO_ENUM_NO_MORE_DEVICES;
    }

    if (!pChildDescriptor || ChildEnumInfo->ChildDescriptorSize < 128)
        return VIDEO_ENUM_INVALID_DEVICE;
    /* XP's monitor child descriptor is the validated base EDID block. */
    VideoPortMoveMemory(pChildDescriptor, dev->MonitorEdid[index], 128);
    *VideoChildType = Monitor;
    *UId = dev->MonitorIds[index];
    LogStr("NVX [DISPLAY]: enumerate monitor id=");
    LogHex(*UId);
    LogStr(" index=");
    LogHex(index);
    LogStr("\n");
    return VIDEO_ENUM_MORE_DEVICES;
}

static VOID NvxInitSecondaryDisplayModes(PNVX_DEVICE_EXTENSION sec) {
    PNVX_DEVICE_EXTENSION prim;
    VIDEO_MODE_INFORMATION *m;
    ULONG idx = 0;
    ULONG edidW = 0, edidH = 0, edidHz = 60;
    if (!sec || !sec->IsSecondaryDisplay || sec->NumModes > 0) return;
    prim = (PNVX_DEVICE_EXTENSION)sec->PrimaryDeviceExtension;
    if (!prim) return;

    /* If EDID detailed timing was discovered for secondary head, prioritize it as Mode 0 */
    if (sec->MonitorTiming[0].hActive >= 640 && sec->MonitorTiming[0].vActive >= 480) {
        edidW = sec->MonitorTiming[0].hActive;
        edidH = sec->MonitorTiming[0].vActive;
        if (sec->MonitorTiming[0].refreshMilliHz >= 20000)
            edidHz = (sec->MonitorTiming[0].refreshMilliHz + 500) / 1000;
        LogStr("NVX [DUALVIEW]: Secondary EDID detailed timing found: ");
        LogHex(edidW); LogStr("x"); LogHex(edidH); LogStr("@"); LogHex(edidHz); LogStr("Hz\n");
    }

    /* Standard resolution candidates for secondary display */
    static const struct {
        ULONG w, h;
    } standardModes[] = {
        { 2560, 1440 },
        { 1920, 1080 },
        { 1920, 1200 },
        { 3840, 2160 },
        { 1680, 1050 },
        { 1600, 900 },
        { 1440, 900 },
        { 1366, 768 },
        { 1280, 1024 },
        { 1280, 720 },
        { 1024, 768 },
        { 800, 600 },
        { 640, 480 }
    };

    /* Add EDID preferred native mode first if present */
    if (edidW && edidH) {
        m = &sec->Modes[idx];
        VideoPortZeroMemory(m, sizeof(*m));
        m->Length = sizeof(*m);
        m->ModeIndex = idx;
        m->VisScreenWidth = edidW;
        m->VisScreenHeight = edidH;
        m->ScreenStride = edidW * 4;
        m->NumberOfPlanes = 1;
        m->BitsPerPlane = 32;
        m->Frequency = edidHz;
        m->NumberRedBits = 8;
        m->NumberGreenBits = 8;
        m->NumberBlueBits = 8;
        m->RedMask = 0x00FF0000;
        m->GreenMask = 0x0000FF00;
        m->BlueMask = 0x000000FF;
        m->AttributeFlags = VIDEO_MODE_COLOR | VIDEO_MODE_GRAPHICS;
        m->VideoMemoryBitmapWidth = edidW;
        m->VideoMemoryBitmapHeight = edidH;
        sec->ModeFramebuffer[idx].QuadPart = prim->PhysicalBar1.QuadPart + 0x2000000ULL; /* +32MB */
        idx++;
    }

    /* Populate standard DMT/VESA modes (skipping duplicate of EDID mode) */
    for (ULONG i = 0; i < sizeof(standardModes)/sizeof(standardModes[0]) && idx < MAX_NVX_MODES; i++) {
        if (standardModes[i].w == edidW && standardModes[i].h == edidH) continue;
        m = &sec->Modes[idx];
        VideoPortZeroMemory(m, sizeof(*m));
        m->Length = sizeof(*m);
        m->ModeIndex = idx;
        m->VisScreenWidth = standardModes[i].w;
        m->VisScreenHeight = standardModes[i].h;
        m->ScreenStride = standardModes[i].w * 4;
        m->NumberOfPlanes = 1;
        m->BitsPerPlane = 32;
        m->Frequency = 60;
        m->NumberRedBits = 8;
        m->NumberGreenBits = 8;
        m->NumberBlueBits = 8;
        m->RedMask = 0x00FF0000;
        m->GreenMask = 0x0000FF00;
        m->BlueMask = 0x000000FF;
        m->AttributeFlags = VIDEO_MODE_COLOR | VIDEO_MODE_GRAPHICS;
        m->VideoMemoryBitmapWidth = standardModes[i].w;
        m->VideoMemoryBitmapHeight = standardModes[i].h;
        sec->ModeFramebuffer[idx].QuadPart = prim->PhysicalBar1.QuadPart + 0x2000000ULL; /* +32MB */
        idx++;
    }

    sec->NumModes = idx;
    sec->CurrentModeIndex = 0;
    sec->ActiveBpp = 32;
    LogStr("NVX [DUALVIEW]: Initialized secondary modes count: ");
    LogHex(sec->NumModes);
    LogStr(" preferred native: ");
    LogHex(sec->Modes[0].VisScreenWidth);
    LogStr("x");
    LogHex(sec->Modes[0].VisScreenHeight);
    LogStr("\n");
}

static VOID NvxInitPrimaryDisplayModes(PNVX_DEVICE_EXTENSION devExt) {
    VIDEO_MODE_INFORMATION *m;
    ULONG idx = 0;
    ULONG edidW = 0, edidH = 0, edidHz = 60;
    if (!devExt || devExt->IsSecondaryDisplay || devExt->NumModes > 0) return;

    /* Check if Head 0 EDID detailed timing is available */
    if (devExt->MonitorTiming[0].hActive >= 640 && devExt->MonitorTiming[0].vActive >= 480) {
        edidW = devExt->MonitorTiming[0].hActive;
        edidH = devExt->MonitorTiming[0].vActive;
        if (devExt->MonitorTiming[0].refreshMilliHz >= 20000)
            edidHz = (devExt->MonitorTiming[0].refreshMilliHz + 500) / 1000;
        LogStr("NVX [NATIVE]: Primary EDID detailed timing found: ");
        LogHex(edidW); LogStr("x"); LogHex(edidH); LogStr("@"); LogHex(edidHz); LogStr("Hz\n");
    }

    static const struct {
        ULONG w, h;
    } standardModes[] = {
        { 1920, 1080 },
        { 2560, 1440 },
        { 1920, 1200 },
        { 3840, 2160 },
        { 1680, 1050 },
        { 1600, 900 },
        { 1440, 900 },
        { 1366, 768 },
        { 1280, 1024 },
        { 1280, 720 },
        { 1024, 768 },
        { 800, 600 },
        { 640, 480 }
    };

    /* Add EDID preferred native mode first if present */
    if (edidW && edidH) {
        m = &devExt->Modes[idx];
        VideoPortZeroMemory(m, sizeof(*m));
        m->Length = sizeof(*m);
        m->ModeIndex = idx;
        m->VisScreenWidth = edidW;
        m->VisScreenHeight = edidH;
        m->ScreenStride = edidW * 4;
        m->NumberOfPlanes = 1;
        m->BitsPerPlane = 32;
        m->Frequency = edidHz;
        m->NumberRedBits = 8;
        m->NumberGreenBits = 8;
        m->NumberBlueBits = 8;
        m->RedMask = 0x00FF0000;
        m->GreenMask = 0x0000FF00;
        m->BlueMask = 0x000000FF;
        m->AttributeFlags = VIDEO_MODE_COLOR | VIDEO_MODE_GRAPHICS;
        m->VideoMemoryBitmapWidth = edidW;
        m->VideoMemoryBitmapHeight = edidH;
        devExt->ModeFramebuffer[idx].QuadPart = devExt->PhysicalBar1.QuadPart;
        idx++;
    }

    /* Populate standard DMT/VESA modes (skipping duplicate of EDID mode) */
    for (ULONG i = 0; i < sizeof(standardModes)/sizeof(standardModes[0]) && idx < MAX_NVX_MODES; i++) {
        if (standardModes[i].w == edidW && standardModes[i].h == edidH) continue;
        m = &devExt->Modes[idx];
        VideoPortZeroMemory(m, sizeof(*m));
        m->Length = sizeof(*m);
        m->ModeIndex = idx;
        m->VisScreenWidth = standardModes[i].w;
        m->VisScreenHeight = standardModes[i].h;
        m->ScreenStride = standardModes[i].w * 4;
        m->NumberOfPlanes = 1;
        m->BitsPerPlane = 32;
        m->Frequency = 60;
        m->NumberRedBits = 8;
        m->NumberGreenBits = 8;
        m->NumberBlueBits = 8;
        m->RedMask = 0x00FF0000;
        m->GreenMask = 0x0000FF00;
        m->BlueMask = 0x000000FF;
        m->AttributeFlags = VIDEO_MODE_COLOR | VIDEO_MODE_GRAPHICS;
        m->VideoMemoryBitmapWidth = standardModes[i].w;
        m->VideoMemoryBitmapHeight = standardModes[i].h;
        devExt->ModeFramebuffer[idx].QuadPart = devExt->PhysicalBar1.QuadPart;
        idx++;
    }

    devExt->NumModes = idx;
    devExt->CurrentModeIndex = 0;
    devExt->ActiveBpp = 32;
    devExt->ActiveWidth = devExt->Modes[0].VisScreenWidth;
    devExt->ActiveHeight = devExt->Modes[0].VisScreenHeight;
    LogStr("NVX [NATIVE]: Initialized primary modes count: ");
    LogHex(devExt->NumModes);
    LogStr(" preferred native: ");
    LogHex(devExt->ActiveWidth);
    LogStr("x");
    LogHex(devExt->ActiveHeight);
    LogStr("\n");
}

VP_STATUS NTAPI HwVidFindAdapter(
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

    NvxLogFlush();
    if (Again) *Again = 0;

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

    /* Read PCI Device ID and query Upstream OpenRM Database */
    {
        USHORT pciDev[2] = {0, 0};
        UCHAR pciRevision = 0;
        VideoPortGetBusData(HwDeviceExtension, PCIConfiguration, 0, pciDev, 0x00, sizeof(pciDev));
        VideoPortGetBusData(HwDeviceExtension, PCIConfiguration, 0,
                            &pciRevision, 0x08, sizeof(pciRevision));
        devExt->DeviceId=pciDev[1];
        devExt->RevisionId=pciRevision;
        LogStr("NVX: PCI Vendor: 0x");
        LogHex(pciDev[0]);
        LogStr(" Device: 0x");
        LogHex(pciDev[1]);
        if (pciDev[0] == 0x10DE) {
            const char *gpuName = NvxLookupGpuName(pciDev[1]);
            if (gpuName) {
                LogStr(" ('");
                LogStr(gpuName);
                LogStr("')");
            }
        }
        LogStr("\n");
    }

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

    if (!devExt->IsSecondaryDisplay) {
        devExt->SecondaryDeviceExtension = NULL;
        status = VideoPortCreateSecondaryDisplay(
            HwDeviceExtension,
            &devExt->SecondaryDeviceExtension,
            VIDEO_DUALVIEW_REMOVABLE
        );
        LogStr("NVX [DUALVIEW]: VideoPortCreateSecondaryDisplay status: ");
        LogHex(status);
        LogStr(" SecDevExt: ");
        LogHex((unsigned long long)devExt->SecondaryDeviceExtension);
        LogStr("\n");
        if (status == NO_ERROR && devExt->SecondaryDeviceExtension) {
            PNVX_DEVICE_EXTENSION sec = (PNVX_DEVICE_EXTENSION)devExt->SecondaryDeviceExtension;
            sec->IsSecondaryDisplay = TRUE;
            sec->PrimaryDeviceExtension = devExt;
            sec->PhysicalBar0 = devExt->PhysicalBar0;
            sec->Bar0Length = devExt->Bar0Length;
            sec->PhysicalBar1 = devExt->PhysicalBar1;
            sec->Bar1Length = devExt->Bar1Length;
            sec->PciSlot = devExt->PciSlot;
            sec->PciBusNumber = devExt->PciBusNumber;
            sec->ActiveBpp = 32;
            if (devExt->MonitorCount >= 2) {
                sec->MonitorCount = 1;
                sec->MonitorIds[0] = devExt->MonitorIds[1];
                sec->MonitorEdidBytes[0] = devExt->MonitorEdidBytes[1];
                sec->MonitorTiming[0] = devExt->MonitorTiming[1];
                VideoPortMoveMemory(sec->MonitorEdid[0], devExt->MonitorEdid[1], sizeof(sec->MonitorEdid[0]));
            }
            NvxInitSecondaryDisplayModes(sec);
        }
    }

    LogStr("NVX: HwVidFindAdapter returning NO_ERROR\n");
    NvxLogFlush();
    return NO_ERROR;
}

BOOLEAN NTAPI HwVidInitialize(PVOID HwDeviceExtension) {
    PNVX_DEVICE_EXTENSION devExt = (PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    VP_STATUS enumStatus;

    NvxLogFlush();
    LogStr("NVX: HwVidInitialize entered, IsSecondary: ");
    LogHex(devExt->IsSecondaryDisplay);
    LogStr("\n");

    if (devExt->IsSecondaryDisplay) {
        PNVX_DEVICE_EXTENSION prim = (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension;
        NvxInitSecondaryDisplayModes(devExt);
        LogStr("NVX [DUALVIEW]: Secondary initialized with ");
        LogHex(devExt->NumModes);
        LogStr(" modes\n");
        if (prim && prim->MonitorCount >= 2) {
            devExt->MonitorCount = 1;
            LogStr("NVX [DUALVIEW]: re-enumerating secondary children\n");
            enumStatus = VideoPortEnumerateChildren(HwDeviceExtension, NULL);
            LogStr("NVX [DUALVIEW]: secondary VideoPortEnumerateChildren status=");
            LogHex(enumStatus);
            LogStr("\n");
        }
        NvxLogFlush();
        return TRUE;
    }

#if defined(__VXDKERNEL__) || !defined(_WIN64)
    /*
     * On 32-bit Windows XP, INT10 real-mode services via HAL/BIOS corrupt
     * kernel mode or fail on modern UEFI/PCI GPUs. Bypass VBE and use authentic
     * GA104 native display modes directly.
     */
    LogStr("NVX [NATIVE]: Native GA104 Direct Modeset Selected (VBE bypassed)\n");
    NvxInitPrimaryDisplayModes(devExt);
#else
    /* Read real-mode information through VideoPort's INT10 service. If VBE INT10
       is unavailable or rejected (e.g. on 32-bit HAL where INT10 write fails),
       fall back directly to native primary display modes rather than failing init. */
    if (!NvxVbeInitialize(HwDeviceExtension)) {
        LogStr("NVX [VBE]: VBE INT10 unavailable or rejected; falling back directly to native primary display modes.\n");
        NvxInitPrimaryDisplayModes(devExt);
    } else if (devExt->NumModes <= 4) {
        LogStr("NVX [NATIVE]: VBE reported degraded mode list (<=4 modes); upgrading to full native/DMT modes\n");
        devExt->NumModes = 0;
        NvxInitPrimaryDisplayModes(devExt);
    }
#endif

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

#if NVX_ENABLE_EXPERIMENTAL_GSP && !defined(__VXDKERNEL__)
#if NVX_ENABLE_UNSAFE_GSP_PROBE
        {
            BOOLEAN initOk=NvxGspInitSubsystem(HwDeviceExtension);
            LogStr("NVX [GSP-STATE]: init_return=");LogHex(initOk);LogStr("\n");
            if (!initOk) {
                LogStr("NVX [GSP]: Bootstrap incomplete; restoring verified VBE mode.\n");
                if (!NvxVbeSetMode(HwDeviceExtension, devExt->CurrentModeIndex))
                    LogStr("NVX [GSP]: WARNING: VBE mode restore failed.\n");
            }
        }
#else
        LogStr("NVX [GSP]: Validating and executing board FWSEC/FRTS before Booter Load.\n");
        NvxProbeFwsecRom(HwDeviceExtension,(PUCHAR)devExt->Bar0MmioVirtual);
        if (devExt->FwsecFrtsReady) {
            LogStr("NVX [GSP]: Verified FWSEC/FRTS prerequisite; starting GSP bootstrap.\n");
            {
                BOOLEAN initOk=NvxGspInitSubsystem(HwDeviceExtension);
                LogStr("NVX [GSP-STATE]: init_return=");LogHex(initOk);LogStr("\n");
                if (!initOk) {
                    LogStr("NVX [GSP]: Bootstrap incomplete; staging retained for diagnosis.\n");
                }
            }
        }
#endif
#elif defined(__VXDKERNEL__)
        LogStr("NVX [GSP]: Win98 VxD deferred GSP bootstrap (scheduled for Init_Complete)\n");
#else
        LogStr("NVX [GSP]: Experimental bootstrap disabled; preserving BIOS scanout.\n");
#endif
    }

    LogStr("NVX [GSP-STATE]: after-init fwsec_frts=");
    LogHex(devExt->FwsecFrtsReady);
    LogStr(" boot_verified=");LogHex(devExt->GspBootVerified);
    LogStr(" initialized=");LogHex(devExt->GspInitialized);
    LogStr(" copy_verified=");LogHex(devExt->GspCopyVerified);
    LogStr(" channel_faulted=");LogHex(devExt->GspChannelFaulted);
    LogStr(" fb_layout=");LogHex(devExt->GspFbLayoutValid);
    LogStr(" scanout_mapped=");LogHex(devExt->GspScanoutMapped);
    LogStr(" gpu_va=");LogHex(devExt->GspVramGpuVa);LogStr("\n");

    /* Match the working order: BIOS selects the primary desktop mode
       before creating native channels.
       Initializing here lets the later VBE modeset overwrite live state. */
    if (devExt->GspCopyVerified)
        LogStr("NVX [NATIVE]: channel initialization deferred until after primary BIOS modeset.\n");

    /* Connector/EDID discovery runs after VideoPort's initial PnP child query.
       Ask XP to enumerate again now that MonitorCount and EDIDs are populated. */
    if (devExt->MonitorCount) {
        LogStr("NVX [DISPLAY]: re-enumerating children after discovery; count=");
        LogHex(devExt->MonitorCount);
        LogStr("\n");
        enumStatus=VideoPortEnumerateChildren(HwDeviceExtension,NULL);
        LogStr("NVX [DISPLAY]: VideoPortEnumerateChildren status=");
        LogHex(enumStatus);
        LogStr("\n");
        if (devExt->SecondaryDeviceExtension && devExt->MonitorCount >= 2) {
            PNVX_DEVICE_EXTENSION sec = (PNVX_DEVICE_EXTENSION)devExt->SecondaryDeviceExtension;
            sec->MonitorCount = 1;
            LogStr("NVX [DUALVIEW]: re-enumerating secondary children from primary init\n");
            enumStatus = VideoPortEnumerateChildren(devExt->SecondaryDeviceExtension, NULL);
            LogStr("NVX [DUALVIEW]: secondary VideoPortEnumerateChildren status=");
            LogHex(enumStatus);
            LogStr("\n");
        }
    }

    LogStr("NVX: HwVidInitialize returning TRUE\n");
    NvxLogFlush();
    return TRUE;
}

static volatile ULONG g_Nvx3dBufferLock=0;
static volatile LONG g_Nvx3dProcessContextLogged=0;
static ULONG g_Nvx3dSessionSeed=0;
static ULONG g_Nvx3dSessionSequence=0;

static BOOLEAN Nvx3dLockBuffers(VOID)
{
    ULONG wait;
    for(wait=0;wait<100000;wait++) {
        if(!__sync_lock_test_and_set(&g_Nvx3dBufferLock,1)) return TRUE;
        VideoPortStallExecution(1);
    }
    return FALSE;
}

static VOID Nvx3dUnlockBuffers(VOID)
{
    __sync_lock_release(&g_Nvx3dBufferLock);
}

static BOOLEAN Nvx3dClientSessionActiveLocked(
    PNVX_DEVICE_EXTENSION engine,ULONG64 clientToken)
{
    ULONG i;
    if(!engine||!clientToken) return FALSE;
    for(i=0;i<NVX_3D_CLIENT_SESSION_SLOTS;i++)
        if(engine->Nvx3dClientSessions[i].ClientToken==clientToken)
            return TRUE;
    return FALSE;
}

static ULONG64 Nvx3dMaxSurfaceBytes(PNVX_DEVICE_EXTENSION dev)
{
    /* Only the currently allocated scanout occupies framebuffer memory.
       Reserving the largest advertised mode for both heads can consume more
       than the mapped VRAM window even though those modes are never allocated
       simultaneously, incorrectly disabling all user allocations. */
    if(!dev||dev->CurrentModeIndex>=dev->NumModes||
       dev->CurrentModeIndex>=MAX_NVX_MODES) return 0;
    return (ULONG64)dev->Modes[dev->CurrentModeIndex].ScreenStride*
           dev->Modes[dev->CurrentModeIndex].VisScreenHeight;
}

static BOOLEAN Nvx3dHeapBounds(PNVX_DEVICE_EXTENSION engine,
                               ULONG64 *start,ULONG64 *end)
{
    PNVX_DEVICE_EXTENSION primary,secondary;
    ULONG64 reserved,aligned,limit,secondaryBytes;
    if(!engine||!start||!end||!engine->GspFbLayoutValid||
       !engine->GspVramGpuVa||!engine->GspVramMapBytes||
       engine->GspFbFreeLimit==~0ULL) return FALSE;
    primary=engine->IsSecondaryDisplay?
        (PNVX_DEVICE_EXTENSION)engine->PrimaryDeviceExtension:engine;
    secondary=primary?(PNVX_DEVICE_EXTENSION)primary->SecondaryDeviceExtension:NULL;
    reserved=Nvx3dMaxSurfaceBytes(primary);
    secondaryBytes=Nvx3dMaxSurfaceBytes(secondary);
    if(secondaryBytes && 0x2000000ULL+secondaryBytes>reserved)
        reserved=0x2000000ULL+secondaryBytes;
    if(reserved<engine->GspFbFreeBase) reserved=engine->GspFbFreeBase;
    if(reserved>~0ULL-0xffffULL) return FALSE;
    aligned=(reserved+0xffffULL)&~0xffffULL;
    limit=engine->GspFbFreeLimit+1;
    if(limit>engine->GspVramMapBytes) limit=engine->GspVramMapBytes;
    if(aligned>=limit) return FALSE;
    *start=aligned;*end=limit;
    return TRUE;
}

static PNVX_3D_USER_BUFFER_ENTRY Nvx3dFindBuffer(PNVX_DEVICE_EXTENSION engine,
                                                ULONG64 id)
{
    ULONG i;
    for(i=0;i<NVX_3D_USER_BUFFER_SLOTS;i++)
        if(engine->Nvx3dBuffers[i].Id==id) return &engine->Nvx3dBuffers[i];
    return NULL;
}

static BOOLEAN Nvx3dFillBuffer(PNVX_DEVICE_EXTENSION engine,ULONG64 gpuVa,
                               ULONG bytes,ULONG usage)
{
    ULONG offset=0,depthEnd=bytes;
    BOOLEAN depth=(usage&NVX_3D_USAGE_DEPTH_TARGET)!=0;
    while(offset<bytes) {
        ULONG chunk=bytes-offset;
        if(depth&&offset>=NVX_3D_DEPTH_TARGET_OFFSET&&offset<depthEnd) {
            offset=depthEnd;
            continue;
        }
        if(chunk>0x01000000U) chunk=0x01000000U;
        if(depth&&offset<NVX_3D_DEPTH_TARGET_OFFSET&&
           chunk>NVX_3D_DEPTH_TARGET_OFFSET-offset)
            chunk=NVX_3D_DEPTH_TARGET_OFFSET-offset;
        if(!chunk||!NvxGspSubmitFill(engine,gpuVa+offset,0,chunk))
            return FALSE;
        offset+=chunk;
    }
    return TRUE;
}

/* The caller holds the global buffer lock. Publish the allocation only after
 * the common buffer is mapped and the RM TLB invalidate is confirmed. */
static BOOLEAN Nvx3dAllocateGartBuffer(PNVX_DEVICE_EXTENSION engine,
    NVX_3D_ALLOC_BUFFER *request,ULONG slot)
{
    NVX_3D_USER_BUFFER_ENTRY *entry=&engine->Nvx3dBuffers[slot];
    PHYSICAL_ADDRESS physical;
    PVOID virtualAddress;
    uint32_t firstSlice,sliceCount;
    ULONG64 gpuVa,id;
    BOOLEAN mapped;

    if(!engine->GspGartReady||!engine->DmaAdapter||
       !NvxGartReserve(&engine->GspGartHeap,request->Bytes,&firstSlice,
                       &sliceCount,&gpuVa)) {
        Nvx3dUnlockBuffers();
        return FALSE;
    }
    engine->Nvx3dNextBufferId++;
    if(!engine->Nvx3dNextBufferId) engine->Nvx3dNextBufferId++;
    id=engine->Nvx3dNextBufferId;
    VideoPortZeroMemory(entry,sizeof(*entry));
    entry->Id=id;
    entry->GpuVa=gpuVa;
    entry->Bytes=request->Bytes;
    entry->Usage=NVX_3D_USAGE_GART_BUFFER;
    entry->GartFirstSlice=firstSlice;
    entry->GartSliceCount=sliceCount;
    entry->Busy=TRUE;
    physical.QuadPart=0;
    Nvx3dUnlockBuffers();

    virtualAddress=VideoPortAllocateCommonBuffer(engine,engine->DmaAdapter,
        request->Bytes,&physical,FALSE,NULL);
    if(!virtualAddress||physical.QuadPart<0||
       ((ULONG64)physical.QuadPart&0xfffULL)||
       (ULONG64)physical.QuadPart>0x100000000ULL-request->Bytes) {
        if(virtualAddress)
            VideoPortFreeCommonBuffer(engine,request->Bytes,virtualAddress,
                                      physical,FALSE);
        if(Nvx3dLockBuffers()) {
            entry=Nvx3dFindBuffer(engine,id);
            if(entry&&entry->Busy&&!entry->SystemVirtual) {
                (void)NvxGartRelease(&engine->GspGartHeap,firstSlice,sliceCount);
                VideoPortZeroMemory(entry,sizeof(*entry));
            }
            Nvx3dUnlockBuffers();
        }
        request->BufferId=request->GpuVa=0;
        return FALSE;
    }

    VideoPortZeroMemory(virtualAddress,request->Bytes);
    /* Busy and ClientToken==0 keep this backing private until map completion. */
    entry->SystemVirtual=virtualAddress;
    entry->SystemPhysical=physical;
    mapped=NvxGspMapGartRange(engine,firstSlice,sliceCount,
                              (ULONG64)physical.QuadPart,request->Bytes);
    if(!mapped) {
        if(NvxGspUnmapGartRange(engine,firstSlice,sliceCount)) {
            if(Nvx3dLockBuffers()) {
                entry=Nvx3dFindBuffer(engine,id);
                if(entry&&entry->Busy&&entry->SystemVirtual==virtualAddress) {
                    VideoPortFreeCommonBuffer(engine,request->Bytes,
                                              virtualAddress,physical,FALSE);
                    (void)NvxGartRelease(&engine->GspGartHeap,
                                         firstSlice,sliceCount);
                    VideoPortZeroMemory(entry,sizeof(*entry));
                }
                Nvx3dUnlockBuffers();
            }
        } else {
            LogStr("NVX [GART]: map invalidate failed; retaining BO backing and VA id=");
            LogHex(id);LogStr(" gpuva=");LogHex(gpuVa);LogStr("\n");
        }
        request->BufferId=request->GpuVa=0;
        return FALSE;
    }

    if(!Nvx3dLockBuffers()) {
        /* Leave the mapping and backing retained; exposing or freeing it
         * without committing ownership would be unsafe. */
        request->BufferId=request->GpuVa=0;
        return FALSE;
    }
    entry=Nvx3dFindBuffer(engine,id);
    if(!entry||!entry->Busy||entry->SystemVirtual!=virtualAddress) {
        Nvx3dUnlockBuffers();
        request->BufferId=request->GpuVa=0;
        return FALSE;
    }
    entry->ClientToken=request->ClientToken;
    entry->AllocationToken=request->AllocationToken;
    entry->Busy=FALSE;
    request->BufferId=id;
    request->GpuVa=gpuVa;
    Nvx3dUnlockBuffers();
    LogStr("NVX [GART]: committed client BO id=");LogHex(id);
    LogStr(" va=");LogHex(gpuVa);LogStr(" bytes=");LogHex(request->Bytes);
    LogStr(" slices=");LogHex(sliceCount);LogStr("\n");
    return TRUE;
}

static BOOLEAN Nvx3dAllocateBuffer(PNVX_DEVICE_EXTENSION engine,
    NVX_3D_ALLOC_BUFFER *request)
{
    ULONG i,slot=NVX_3D_USER_BUFFER_SLOTS;
    ULONG64 start,end,candidate;
    if(!engine||!request||!Nvx3dValidateAlloc(request)||
       !request->AllocationToken||
       !engine->GspCopyVerified||engine->GspChannelFaulted||
       !Nvx3dLockBuffers()) {
        LogStr("NVX [3D-ALLOC]: request rejected before heap scan.\n");
        return FALSE;
    }
    if(!Nvx3dClientSessionActiveLocked(engine,request->ClientToken)) {
        LogStr("NVX [3D-ALLOC]: inactive client session token=");
        LogHex(request->ClientToken);LogStr("\n");
        Nvx3dUnlockBuffers();
        return FALSE;
    }
    if(request->ClientToken) {
        for(i=0;i<NVX_3D_USER_BUFFER_SLOTS;i++) {
            NVX_3D_USER_BUFFER_ENTRY *entry=&engine->Nvx3dBuffers[i];
            if(entry->Id&&entry->ClientToken==request->ClientToken&&
               entry->AllocationToken==request->AllocationToken) {
                if(entry->Bytes!=request->Bytes||entry->Usage!=request->Usage||
                   entry->Width!=request->Width||entry->Height!=request->Height||
                   entry->PitchBytes!=request->PitchBytes||
                   entry->TileMode!=request->TileMode) {
                    Nvx3dUnlockBuffers();return FALSE;
                }
                request->BufferId=entry->Id;request->GpuVa=entry->GpuVa;
                LogStr("NVX [3D-ALLOC]: replay token=");
                LogHex(request->ClientToken);LogStr(" id=");
                LogHex(request->BufferId);LogStr("\n");
                Nvx3dUnlockBuffers();return TRUE;
            }
        }
    }
    if(request->Usage&NVX_3D_USAGE_DEPTH_TARGET) {
        for(i=0;i<NVX_3D_USER_BUFFER_SLOTS;i++) {
            NVX_3D_USER_BUFFER_ENTRY *entry=&engine->Nvx3dBuffers[i];
            if(entry->Id&&!entry->Busy&&
               (entry->Usage&NVX_3D_USAGE_DEPTH_TARGET)&&
               entry->Bytes==request->Bytes&&entry->Usage==request->Usage&&
               entry->Width==request->Width&&entry->Height==request->Height&&
               entry->PitchBytes==request->PitchBytes&&
               entry->TileMode==request->TileMode) {
                /* A pooled depth allocation is already initialized and idle.
                   Busy protects an in-progress kernel operation; returning it
                   busy would make the caller's first submit fail validation. */
                entry->ClientToken=request->ClientToken;
                entry->AllocationToken=request->AllocationToken;
                request->BufferId=entry->Id;request->GpuVa=entry->GpuVa;
                LogStr("NVX [3D-ALLOC]: reused depth pool id=");
                LogHex(entry->Id);LogStr(" va=");LogHex(entry->GpuVa);
                LogStr("\n");
                Nvx3dUnlockBuffers();return TRUE;
            }
        }
    }
    for(i=0;i<NVX_3D_USER_BUFFER_SLOTS;i++)
        if(!engine->Nvx3dBuffers[i].Id) {slot=i;break;}
    if(slot==NVX_3D_USER_BUFFER_SLOTS) {
        LogStr("NVX [3D-ALLOC]: user buffer table is full.\n");
        Nvx3dUnlockBuffers();return FALSE;
    }
    if(request->Usage&NVX_3D_USAGE_GART_BUFFER)
        return Nvx3dAllocateGartBuffer(engine,request,slot);
    if(!Nvx3dHeapBounds(engine,&start,&end)) {
        Nvx3dUnlockBuffers();return FALSE;
    }
    /* The GPU VRAM mapping uses 64 KiB leaf PTEs. A depth target may change
       the PTE kind for its entire leaf, so keep its buffer 64 KiB aligned and
       reserve the depth page without sharing that leaf with the color target. */
    candidate=start;
    for(;;) {
        BOOLEAN moved=FALSE;
        if(candidate>end||request->Bytes>end-candidate) {
            LogStr("NVX [3D-ALLOC]: VRAM heap exhausted bytes=");
            LogHex(request->Bytes);LogStr(" candidate=");LogHex(candidate);
            LogStr(" end=");LogHex(end);LogStr("\n");
            Nvx3dUnlockBuffers();return FALSE;
        }
        for(i=0;i<NVX_3D_USER_BUFFER_SLOTS;i++) {
            NVX_3D_USER_BUFFER_ENTRY *entry=&engine->Nvx3dBuffers[i];
            ULONG64 entryStart,entryEnd;
            if(!entry->Id) continue;
            entryStart=entry->GpuVa-engine->GspVramGpuVa;
            entryEnd=entryStart+entry->Bytes;
            if(candidate<entryEnd&&entryStart<candidate+request->Bytes) {
                candidate=(entryEnd+0xffffULL)&~0xffffULL;
                moved=TRUE;break;
            }
        }
        if(!moved) break;
    }
    if(request->Usage&NVX_3D_USAGE_DEPTH_TARGET)
        candidate=(candidate+0xffffULL)&~0xffffULL;
    if(candidate>end||request->Bytes>end-candidate) {
        LogStr("NVX [3D-ALLOC]: aligned VRAM range exhausted bytes=");
        LogHex(request->Bytes);LogStr(" candidate=");LogHex(candidate);
        LogStr(" end=");LogHex(end);LogStr("\n");
        Nvx3dUnlockBuffers();return FALSE;
    }
    engine->Nvx3dNextBufferId++;
    if(!engine->Nvx3dNextBufferId) engine->Nvx3dNextBufferId++;
    engine->Nvx3dBuffers[slot].Id=engine->Nvx3dNextBufferId;
    engine->Nvx3dBuffers[slot].GpuVa=engine->GspVramGpuVa+candidate;
    engine->Nvx3dBuffers[slot].DepthGpuVa=
        (request->Usage&NVX_3D_USAGE_DEPTH_TARGET)?
        engine->GspVramGpuVa+candidate+NVX_3D_DEPTH_TARGET_OFFSET:0;
    engine->Nvx3dBuffers[slot].ClientToken=request->ClientToken;
    engine->Nvx3dBuffers[slot].AllocationToken=request->AllocationToken;
    engine->Nvx3dBuffers[slot].Bytes=request->Bytes;
    engine->Nvx3dBuffers[slot].Usage=request->Usage;
    engine->Nvx3dBuffers[slot].Width=request->Width;
    engine->Nvx3dBuffers[slot].Height=request->Height;
    engine->Nvx3dBuffers[slot].PitchBytes=request->PitchBytes;
    engine->Nvx3dBuffers[slot].TileMode=request->TileMode;
    engine->Nvx3dBuffers[slot].Busy=TRUE;
    request->BufferId=engine->Nvx3dBuffers[slot].Id;
    request->GpuVa=engine->Nvx3dBuffers[slot].GpuVa;
    Nvx3dUnlockBuffers();
    if((request->Usage&NVX_3D_USAGE_DEPTH_TARGET) &&
       (request->Bytes<NVX_3D_DEPTH_TARGET_TOTAL_BYTES ||
        !NvxGspSetDepthRangeKind(engine,
            request->GpuVa+NVX_3D_DEPTH_TARGET_OFFSET,
            request->Bytes-NVX_3D_DEPTH_TARGET_OFFSET,
            (request->Usage&NVX_3D_USAGE_DEPTH_ZF32)?6U:
            (request->Usage&NVX_3D_USAGE_DEPTH_S8Z24)?3U:5U,TRUE))) {
        if(engine->Nvx3dBuffers[slot].DepthGpuVa)
            (void)NvxGspSetDepthRangeKind(engine,
                request->GpuVa+NVX_3D_DEPTH_TARGET_OFFSET,
                request->Bytes-NVX_3D_DEPTH_TARGET_OFFSET,0U,FALSE);
        if(Nvx3dLockBuffers()) {
            PNVX_3D_USER_BUFFER_ENTRY entry=Nvx3dFindBuffer(engine,request->BufferId);
            if(entry) VideoPortZeroMemory(entry,sizeof(*entry));
            Nvx3dUnlockBuffers();
        }
        request->BufferId=request->GpuVa=0;
        return FALSE;
    }
    if(!Nvx3dFillBuffer(engine,request->GpuVa,request->Bytes,request->Usage)) {
        LogStr("NVX [3D-ALLOC]: initial clear/fill failed id=");
        LogHex(request->BufferId);LogStr("\n");
        if(request->Usage&NVX_3D_USAGE_DEPTH_TARGET)
            (void)NvxGspSetDepthRangeKind(engine,
                request->GpuVa+NVX_3D_DEPTH_TARGET_OFFSET,
                request->Bytes-NVX_3D_DEPTH_TARGET_OFFSET,0U,FALSE);
        if(Nvx3dLockBuffers()) {
            PNVX_3D_USER_BUFFER_ENTRY entry=Nvx3dFindBuffer(engine,request->BufferId);
            if(entry) VideoPortZeroMemory(entry,sizeof(*entry));
            Nvx3dUnlockBuffers();
        }
        request->BufferId=request->GpuVa=0;
        return FALSE;
    }
    if(Nvx3dLockBuffers()) {
        PNVX_3D_USER_BUFFER_ENTRY entry=Nvx3dFindBuffer(engine,request->BufferId);
        if(entry) entry->Busy=FALSE;
        Nvx3dUnlockBuffers();
    }
    engine->GspGraphicsUserAllocReady=TRUE;
    LogStr("NVX [3D-ALLOC]: committed id=");LogHex(request->BufferId);
    LogStr(" slot=");LogHex(slot);LogStr(" engine=");LogHex((ULONG64)engine);
    LogStr(" va=");LogHex(request->GpuVa);LogStr(" usage=");
    LogHex(request->Usage);LogStr("\n");
    return TRUE;
}

static BOOLEAN Nvx3dFreeBuffer(PNVX_DEVICE_EXTENSION engine,ULONG64 id,
                              ULONG64 clientToken)
{
    NVX_3D_USER_BUFFER_ENTRY saved;
    PNVX_3D_USER_BUFFER_ENTRY entry;
    if(!engine||!id||!clientToken||!Nvx3dLockBuffers()) return FALSE;
    entry=Nvx3dFindBuffer(engine,id);
    if(!entry) {
        /* ExtEscape can be replayed by the dual-head display path after the
           first identical free already released the monotonically unique ID. */
        Nvx3dUnlockBuffers();return TRUE;
    }
    if(entry->ClientToken!=clientToken||
       !Nvx3dClientSessionActiveLocked(engine,clientToken)) {
        Nvx3dUnlockBuffers();return FALSE;
    }
    if(entry->Usage&NVX_3D_USAGE_DEPTH_TARGET) {
        if(!entry->Busy) {
            /* A pooled depth surface is no longer owned by the session that
               released it; the next allocation assigns a fresh session. */
            entry->ClientToken=0;
            entry->AllocationToken=0;
            Nvx3dUnlockBuffers();return TRUE;
        }
        saved=*entry;Nvx3dUnlockBuffers();
        if(!Nvx3dLockBuffers()) return FALSE;
        entry=Nvx3dFindBuffer(engine,id);
        if(entry&&entry->Busy&&entry->GpuVa==saved.GpuVa)
            entry->Busy=FALSE;
        Nvx3dUnlockBuffers();
        LogStr("NVX [3D-FREE]: retained depth pool id=");LogHex(id);
        LogStr(" va=");LogHex(saved.GpuVa);LogStr("\n");
        return TRUE;
    }
    if(entry->Busy) {
        if((entry->Usage&NVX_3D_USAGE_GART_BUFFER)&&
           entry->GartUnmapPending) {
            /* Retry only the failed invalidate/unmap; the backing remains
               owned and inaccessible to ordinary map/submit operations. */
            saved=*entry;
            Nvx3dUnlockBuffers();
            if(!NvxGspUnmapGartRange(engine,saved.GartFirstSlice,
                                     saved.GartSliceCount))
                return FALSE;
            if(!Nvx3dLockBuffers()) return FALSE;
            entry=Nvx3dFindBuffer(engine,id);
            if(entry&&entry->Busy&&entry->GartUnmapPending&&
               entry->SystemVirtual==saved.SystemVirtual) {
                VideoPortFreeCommonBuffer(engine,saved.Bytes,
                    saved.SystemVirtual,saved.SystemPhysical,FALSE);
                (void)NvxGartRelease(&engine->GspGartHeap,
                    saved.GartFirstSlice,saved.GartSliceCount);
                VideoPortZeroMemory(entry,sizeof(*entry));
            }
            Nvx3dUnlockBuffers();
            return TRUE;
        }
        LogStr("NVX [3D-FREE]: missing or busy id=");LogHex(id);
        LogStr(" engine=");LogHex((ULONG64)engine);
        LogStr(" missing=");LogHex(entry?0U:1U);
        LogStr(" busy=");LogHex(entry?entry->Busy:0U);LogStr("\n");
        Nvx3dUnlockBuffers();return FALSE;
    }
    entry->Busy=TRUE;saved=*entry;Nvx3dUnlockBuffers();
    LogStr("NVX [3D-FREE]: found id=");LogHex(id);
    LogStr(" engine=");LogHex((ULONG64)engine);
    LogStr(" va=");LogHex(saved.GpuVa);LogStr(" usage=");
    LogHex(saved.Usage);LogStr("\n");
    if(saved.Usage&NVX_3D_USAGE_GART_BUFFER) {
        if(!saved.SystemVirtual||!saved.GartSliceCount) return FALSE;
        VideoPortZeroMemory(saved.SystemVirtual,saved.Bytes);
        __sync_synchronize();
        if(!NvxGspUnmapGartRange(engine,saved.GartFirstSlice,
                                 saved.GartSliceCount)) {
            if(Nvx3dLockBuffers()) {
                entry=Nvx3dFindBuffer(engine,id);
                if(entry&&entry->Busy&&entry->SystemVirtual==saved.SystemVirtual)
                    entry->GartUnmapPending=TRUE;
                Nvx3dUnlockBuffers();
            }
            LogStr("NVX [GART]: TLB invalidate unconfirmed; retaining BO backing id=");
            LogHex(id);LogStr("\n");
            return FALSE;
        }
        if(!Nvx3dLockBuffers()) return FALSE;
        entry=Nvx3dFindBuffer(engine,id);
        if(entry&&entry->Busy&&entry->GpuVa==saved.GpuVa&&
           entry->SystemVirtual==saved.SystemVirtual) {
            VideoPortFreeCommonBuffer(engine,saved.Bytes,
                saved.SystemVirtual,saved.SystemPhysical,FALSE);
            (void)NvxGartRelease(&engine->GspGartHeap,
                saved.GartFirstSlice,saved.GartSliceCount);
            VideoPortZeroMemory(entry,sizeof(*entry));
        }
        Nvx3dUnlockBuffers();
        return TRUE;
    }
    if(!Nvx3dFillBuffer(engine,saved.GpuVa,saved.Bytes,saved.Usage)) {
        LogStr("NVX [3D-FREE]: fill failed id=");LogHex(id);
        LogStr(" usage=");LogHex(saved.Usage);LogStr(" bytes=");
        LogHex(saved.Bytes);LogStr("\n");
        if(Nvx3dLockBuffers()) {
            entry=Nvx3dFindBuffer(engine,id);
            if(entry) entry->Busy=FALSE;
            Nvx3dUnlockBuffers();
        }
        return FALSE;
    }
            if((saved.Usage&NVX_3D_USAGE_DEPTH_TARGET) &&
       !NvxGspSetDepthRangeKind(engine,
           saved.GpuVa+NVX_3D_DEPTH_TARGET_OFFSET,
           saved.Bytes-NVX_3D_DEPTH_TARGET_OFFSET,0U,FALSE)) {
        if(Nvx3dLockBuffers()) {
            entry=Nvx3dFindBuffer(engine,id);
            if(entry) entry->Busy=FALSE;
            Nvx3dUnlockBuffers();
        }
        return FALSE;
    }
    if(!Nvx3dLockBuffers()) {
        LogStr("NVX [3D-FREE]: final lock failed id=");LogHex(id);LogStr("\n");
        return FALSE;
    }
    entry=Nvx3dFindBuffer(engine,id);
    if(entry&&entry->Busy&&entry->GpuVa==saved.GpuVa)
        VideoPortZeroMemory(entry,sizeof(*entry));
    else {
        LogStr("NVX [3D-FREE]: ownership changed id=");LogHex(id);
        LogStr(" found=");LogHex(entry?1U:0U);
        LogStr(" busy=");LogHex(entry?entry->Busy:0U);
        LogStr(" va=");LogHex(entry?entry->GpuVa:0U);
        LogStr(" expected=");LogHex(saved.GpuVa);LogStr("\n");
    }
    Nvx3dUnlockBuffers();
    return TRUE;
}

BOOLEAN NTAPI HwVidStartIO(
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
       RequestPacket->IoControlCode!=IOCTL_NVX_ACCEL_FILL &&
       RequestPacket->IoControlCode!=IOCTL_NVX_GET_LOG) {
        LogStr("NVX: HwVidStartIO IoControlCode: ");
        LogHex(RequestPacket->IoControlCode);
        LogStr("\n");
    }

    statusBlock->Information = 0;

    switch (RequestPacket->IoControlCode) {
    case IOCTL_NVX_3D_CLIENT_OPEN: {
        PNVX_DEVICE_EXTENSION engine=devExt->IsSecondaryDisplay?
            (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension:devExt;
        NVX_3D_CLIENT_SESSION request;
        ULONG i,attempt;
        ULONG64 token=0;
        if(!RequestPacket->InputBuffer||
           RequestPacket->InputBufferLength!=sizeof(request)||
           !RequestPacket->OutputBuffer||
           RequestPacket->OutputBufferLength<sizeof(request)) {
            status=ERROR_INVALID_PARAMETER;break;
        }
        VideoPortMoveMemory(&request,RequestPacket->InputBuffer,sizeof(request));
        if(!Nvx3dValidateClientSession(&request,NVX_3D_CLIENT_OPEN)||
           request.ClientToken||
           !engine||!Nvx3dLockBuffers()) {
            status=ERROR_INVALID_PARAMETER;break;
        }
        for(i=0;i<NVX_3D_CLIENT_SESSION_SLOTS;i++)
            if(!engine->Nvx3dClientSessions[i].ClientToken) break;
        if(i==NVX_3D_CLIENT_SESSION_SLOTS) {
            Nvx3dUnlockBuffers();status=ERROR_TOO_MANY_OPEN_FILES;break;
        }
        if(!g_Nvx3dSessionSeed)
            g_Nvx3dSessionSeed=0x4e565831U^engine->DeviceId^
                (engine->PciBusNumber<<8)^engine->PciSlot;
        for(attempt=0;attempt<32U;attempt++) {
            ULONG high=RtlRandomEx(&g_Nvx3dSessionSeed);
            ULONG low=++g_Nvx3dSessionSequence;
            ULONG j;
            BOOLEAN duplicate=FALSE;
            if(!low) low=++g_Nvx3dSessionSequence;
            token=((ULONG64)high<<32)|low;
            for(j=0;j<NVX_3D_CLIENT_SESSION_SLOTS;j++)
                if(engine->Nvx3dClientSessions[j].ClientToken==token)
                    duplicate=TRUE;
            if(token&&!duplicate) break;
            token=0;
        }
        if(!token) {
            Nvx3dUnlockBuffers();status=ERROR_DEVICE_NOT_READY;break;
        }
        engine->Nvx3dClientSessions[i].ClientToken=token;
        Nvx3dUnlockBuffers();
        request.ClientToken=token;
        VideoPortMoveMemory(RequestPacket->OutputBuffer,&request,sizeof(request));
        statusBlock->Information=sizeof(request);status=NO_ERROR;
        LogStr("NVX [3D-SESSION]: opened token=");LogHex(token);LogStr("\n");
        break;
    }

    case IOCTL_NVX_3D_CLIENT_CLOSE: {
        PNVX_DEVICE_EXTENSION engine=devExt->IsSecondaryDisplay?
            (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension:devExt;
        NVX_3D_CLIENT_SESSION request;
        ULONG i,j;
        BOOLEAN found=FALSE,busy=FALSE;
        if(!RequestPacket->InputBuffer||
           RequestPacket->InputBufferLength!=sizeof(request)||
           RequestPacket->OutputBufferLength) {
            status=ERROR_INVALID_PARAMETER;break;
        }
        VideoPortMoveMemory(&request,RequestPacket->InputBuffer,sizeof(request));
        if(!Nvx3dValidateClientSession(&request,NVX_3D_CLIENT_CLOSE)||
           !engine||!Nvx3dLockBuffers()) {
            status=ERROR_INVALID_PARAMETER;break;
        }
        for(i=0;i<NVX_3D_CLIENT_SESSION_SLOTS;i++)
            if(engine->Nvx3dClientSessions[i].ClientToken==request.ClientToken) {
                found=TRUE;break;
            }
        if(found) {
            for(j=0;j<NVX_3D_USER_BUFFER_SLOTS;j++)
                if(engine->Nvx3dBuffers[j].Id&&
                   engine->Nvx3dBuffers[j].ClientToken==request.ClientToken)
                    busy=TRUE;
        }
        if(found&&!busy) {
            if(engine->Nvx3dChannelOwnerToken==request.ClientToken)
                engine->Nvx3dChannelOwnerToken=0;
            engine->Nvx3dClientSessions[i].ClientToken=0;
        }
        Nvx3dUnlockBuffers();
        status=found&&!busy?NO_ERROR:ERROR_BUSY;
        if(status==NO_ERROR)
            LogStr("NVX [3D-SESSION]: closed client session.\n");
        break;
    }

    case IOCTL_NVX_3D_CHANNEL_LEASE: {
        PNVX_DEVICE_EXTENSION engine=devExt->IsSecondaryDisplay?
            (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension:devExt;
        NVX_3D_CHANNEL_LEASE request;
        if(!RequestPacket->InputBuffer||
           RequestPacket->InputBufferLength!=sizeof(request)||
           !RequestPacket->OutputBuffer||
           RequestPacket->OutputBufferLength<sizeof(request)) {
            status=ERROR_INVALID_PARAMETER;break;
        }
        VideoPortMoveMemory(&request,RequestPacket->InputBuffer,sizeof(request));
        if(!engine||!Nvx3dValidateChannelLease(&request,request.Operation)||
           !Nvx3dLockBuffers()) {
            status=ERROR_INVALID_PARAMETER;break;
        }
        if(!Nvx3dClientSessionActiveLocked(engine,request.ClientToken)) {
            Nvx3dUnlockBuffers();status=ERROR_INVALID_PARAMETER;break;
        }
        if(request.Operation==NVX_3D_CHANNEL_OPEN) {
            if(!engine->GspGraphicsChannelReady||
               !engine->GspGraphicsCommandVerified||
               engine->GspGraphicsChannelFaulted||engine->GspMmuFaultQueued||
               !engine->GspGraphicsChannelHandle||
               !engine->GspGraphicsObjectHandle||
               (engine->Nvx3dChannelOwnerToken&&
                engine->Nvx3dChannelOwnerToken!=request.ClientToken)) {
                Nvx3dUnlockBuffers();status=ERROR_BUSY;break;
            }
            engine->Nvx3dChannelOwnerToken=request.ClientToken;
            request.ChannelHandle=engine->GspGraphicsChannelHandle;
            request.GraphicsObjectHandle=engine->GspGraphicsObjectHandle;
            status=NO_ERROR;
        } else if(request.Operation==NVX_3D_CHANNEL_CLOSE) {
            if(engine->Nvx3dChannelOwnerToken!=request.ClientToken||
               request.ChannelHandle!=engine->GspGraphicsChannelHandle||
               request.GraphicsObjectHandle!=engine->GspGraphicsObjectHandle) {
                Nvx3dUnlockBuffers();status=ERROR_INVALID_PARAMETER;break;
            }
            engine->Nvx3dChannelOwnerToken=0;
            status=NO_ERROR;
        } else {
            Nvx3dUnlockBuffers();status=ERROR_INVALID_PARAMETER;break;
        }
        Nvx3dUnlockBuffers();
        VideoPortMoveMemory(RequestPacket->OutputBuffer,&request,sizeof(request));
        statusBlock->Information=sizeof(request);
        break;
    }

    case IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES: {
        PVIDEO_NUM_MODES numModes = (PVIDEO_NUM_MODES)RequestPacket->OutputBuffer;
        if (RequestPacket->OutputBufferLength < sizeof(VIDEO_NUM_MODES)) {
            status = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        if (devExt->IsSecondaryDisplay && devExt->NumModes == 0) {
            NvxInitSecondaryDisplayModes(devExt);
        } else if (!devExt->IsSecondaryDisplay && devExt->NumModes <= 4) {
            devExt->NumModes = 0;
            NvxInitPrimaryDisplayModes(devExt);
        }
        numModes->NumModes = devExt->NumModes;
        numModes->ModeInformationLength = sizeof(VIDEO_MODE_INFORMATION);
        statusBlock->Information = sizeof(VIDEO_NUM_MODES);
        LogStr("NVX: IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES -> ");
        LogHex(devExt->NumModes);
        LogStr(" modes (IsSec=");
        LogHex(devExt->IsSecondaryDisplay);
        LogStr(")\n");
        break;
    }

    case IOCTL_VIDEO_QUERY_AVAIL_MODES: {
        ULONG requiredSize;
        if (devExt->IsSecondaryDisplay && devExt->NumModes == 0) {
            NvxInitSecondaryDisplayModes(devExt);
        } else if (!devExt->IsSecondaryDisplay && devExt->NumModes <= 4) {
            devExt->NumModes = 0;
            NvxInitPrimaryDisplayModes(devExt);
        }
        requiredSize = devExt->NumModes * sizeof(VIDEO_MODE_INFORMATION);
        if (RequestPacket->OutputBufferLength < requiredSize) {
            status = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        VideoPortMoveMemory(RequestPacket->OutputBuffer, devExt->Modes, requiredSize);
        statusBlock->Information = requiredSize;
        LogStr("NVX: IOCTL_VIDEO_QUERY_AVAIL_MODES returned successfully (IsSec=");
        LogHex(devExt->IsSecondaryDisplay);
        LogStr(")\n");
        break;
    }

    case IOCTL_VIDEO_QUERY_CURRENT_MODE: {
        if (RequestPacket->OutputBufferLength < sizeof(VIDEO_MODE_INFORMATION)) {
            status = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        if (devExt->IsSecondaryDisplay && devExt->NumModes == 0) {
            NvxInitSecondaryDisplayModes(devExt);
        }
        if (devExt->CurrentModeIndex >= devExt->NumModes) {
            devExt->CurrentModeIndex = 0;
        }
        VideoPortMoveMemory(
            RequestPacket->OutputBuffer,
            &devExt->Modes[devExt->CurrentModeIndex],
            sizeof(VIDEO_MODE_INFORMATION)
        );
        statusBlock->Information = sizeof(VIDEO_MODE_INFORMATION);
        LogStr("NVX: IOCTL_VIDEO_QUERY_CURRENT_MODE index=");
        LogHex(devExt->CurrentModeIndex);
        LogStr(" IsSec=");
        LogHex(devExt->IsSecondaryDisplay);
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

        if (devExt->IsSecondaryDisplay) {
            PNVX_DEVICE_EXTENSION prim = (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension;
            devExt->CurrentModeIndex = requested;
            LogStr("NVX [DUALVIEW]: Secondary mode set to index: ");
            LogHex(requested);
            LogStr("\n");
            if (prim) {
                (void)NvxNativeSetSecondaryScanout(prim);
                devExt->ActiveWidth = devExt->Modes[requested].VisScreenWidth;
                devExt->ActiveHeight = devExt->Modes[requested].VisScreenHeight;
                devExt->ActiveBpp = 32;
                devExt->ActiveFreq = devExt->Modes[requested].Frequency;
                status = NO_ERROR;
            } else {
                status = ERROR_INVALID_PARAMETER;
            }
            break;
        }

#ifdef __VXDKERNEL__
        devExt->CurrentModeIndex = requested;
        devExt->ActiveWidth = devExt->Modes[requested].VisScreenWidth;
        devExt->ActiveHeight = devExt->Modes[requested].VisScreenHeight;
        devExt->ActiveBpp = devExt->Modes[requested].BitsPerPlane;
        devExt->ActiveFreq = devExt->Modes[requested].Frequency;
        LogStr("NVX [NATIVE]: Native primary mode switched to index: ");
        LogHex(requested);
        LogStr("\n");
        if (devExt->SecondaryDpTrainReady && !devExt->SecondaryDpTrainingAttempted) {
            NvxGspTrainSecondaryDpLink(HwDeviceExtension);
        }
        break;
#else
        BOOLEAN restoreNativeDual=devExt->NativeDualActive;
        BOOLEAN vbeOk = FALSE;
        /* If mode index is within VBE modes, try VBE first; otherwise switch directly via native scanout */
#ifdef _WIN64
        if (requested < 4) {
            vbeOk = NvxVbeSetMode(HwDeviceExtension, requested);
        }
#endif
        if (!vbeOk) {
            LogStr("NVX [NATIVE]: Direct primary native modeset for index ");
            LogHex(requested);
            LogStr(" (");
            LogHex(devExt->Modes[requested].VisScreenWidth);
            LogStr("x");
            LogHex(devExt->Modes[requested].VisScreenHeight);
            LogStr(")\n");
        }
        devExt->CurrentModeIndex = requested;
        devExt->ActiveWidth = devExt->Modes[requested].VisScreenWidth;
        devExt->ActiveHeight = devExt->Modes[requested].VisScreenHeight;
        devExt->ActiveBpp = devExt->Modes[requested].BitsPerPlane;
        devExt->ActiveFreq = devExt->Modes[requested].Frequency;
        LogStr("NVX: Mode switched to index: ");
        LogHex(requested);
        LogStr("\n");
        if (restoreNativeDual || !vbeOk) {
            (void)NvxNativeSetSecondaryScanout(devExt);
        } else if (devExt->Modes[requested].VisScreenWidth >= 1920 &&
                   devExt->Modes[requested].VisScreenHeight >= 1080 &&
                   devExt->SecondaryDpTrainReady &&
                   !devExt->SecondaryDpTrainingAttempted) {
            NvxGspTrainSecondaryDpLink(HwDeviceExtension);
            /* RM link training can reset display state; reassert the working
               primary VBE mode after it so the secondary cannot leave XP in
               the bootstrap resolution. */
            if (!NvxVbeSetMode(HwDeviceExtension, requested))
                LogStr("NVX [DP]: primary VBE mode restore after link training failed.\n");
        }
        break;
#endif
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
        LogStr(" bpp ");LogHex(devExt->ActiveBpp);
        LogStr(" IsSecondary: ");LogHex(devExt->IsSecondaryDisplay);LogStr("\n");
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
            if (!devExt->IsSecondaryDisplay) {
                (void)NvxGspDiscoverScanout(HwDeviceExtension,devExt->CurrentModeIndex);
                (void)NvxGspInitDisplayChannels(HwDeviceExtension);
#if !defined(_WIN64) && !defined(__VXDKERNEL__)
                /* x86 has no BIOS modeset: channel allocation alone leaves the
                 * boot raster active. Commit the native head/window state only
                 * now, after the primary mapping prerequisite is satisfied. */
                if (!NvxNativeSetSecondaryScanout(devExt)) {
                    LogStr("NVX [NATIVE]: x86 scanout commit failed; rejecting surface mapping.\n");
                    VideoPortUnmapMemory(HwDeviceExtension, mappedVirtual, 0);
                    devExt->FramebufferVirtual = NULL;
                    status = ERROR_DEVICE_NOT_READY;
                    break;
                }
#endif
            } else {
                PNVX_DEVICE_EXTENSION prim = (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension;
                if (prim) {
                    (void)NvxNativeSetSecondaryScanout(prim);
                }
            }
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

    case IOCTL_VIDEO_SWITCH_DUALVIEW: {
        PULONG pSwitch = (PULONG)RequestPacket->InputBuffer;
        ULONG switchVal = (pSwitch && RequestPacket->InputBufferLength >= sizeof(ULONG)) ? *pSwitch : 0;
        LogStr("NVX [DUALVIEW]: IOCTL_VIDEO_SWITCH_DUALVIEW called with val=");
        LogHex(switchVal);
        LogStr(" IsSecondary=");
        LogHex(devExt->IsSecondaryDisplay);
        LogStr("\n");
        devExt->DualViewAttached = (switchVal != 0);
        if (devExt->IsSecondaryDisplay && devExt->NumModes == 0) {
            NvxInitSecondaryDisplayModes(devExt);
        }
        status = NO_ERROR;
        break;
    }

    case IOCTL_VIDEO_GET_CHILD_STATE: {
        PULONG childPtr = (PULONG)RequestPacket->InputBuffer;
        PULONG statePtr = (PULONG)RequestPacket->OutputBuffer;
        ULONG childId;
        ULONG state = VIDEO_CHILD_ACTIVE;
        PNVX_DEVICE_EXTENSION prim = devExt->IsSecondaryDisplay ?
            (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension : devExt;
        ULONG mon0 = (prim && prim->MonitorCount >= 1) ? prim->MonitorIds[0] : 0x0800;
        ULONG mon1 = (prim && prim->MonitorCount >= 2) ? prim->MonitorIds[1] : 0x0200;
        BOOLEAN isLenovo, isDell;

        if (RequestPacket->InputBufferLength < sizeof(ULONG) ||
            RequestPacket->OutputBufferLength < sizeof(ULONG) ||
            !childPtr || !statePtr) {
            status = ERROR_INSUFFICIENT_BUFFER;
            break;
        }

        childId = *childPtr;
        LogStr("NVX [DUALVIEW]: IOCTL_VIDEO_GET_CHILD_STATE child=");
        LogHex(childId);
        LogStr(" IsSec=");
        LogHex(devExt->IsSecondaryDisplay);
        LogStr("\n");

        isLenovo = (childId == 2 || childId == mon1);
        isDell = (childId == 1 || childId == mon0);

        if (devExt->IsSecondaryDisplay) {
            /* Secondary adapter (\Device\Video1):
             * Dell (mon0 / child 1) is inactive on secondary.
             * Lenovo (mon1 / child 2) is the active display on secondary. */
            if (isLenovo) {
                state = VIDEO_CHILD_ACTIVE;
            } else {
                state = 0;
            }
        } else {
            /* Primary adapter (\Device\Video0):
             * Dell (mon0 / child 1) is active on primary.
             * Lenovo (mon1 / child 2) is detached to DualView secondary.
             * Returning ONLY VIDEO_CHILD_DETACHED (without VIDEO_CHILD_ACTIVE)
             * ensures Windows XP does not treat the primary adapter as driving
             * multiple simultaneous displays ("(Multiple Monitors)"), and instead
             * cleanly binds Lenovo to the secondary DualView adapter. */
            if (isDell) {
                state = VIDEO_CHILD_ACTIVE;
            } else if (isLenovo) {
                state = VIDEO_CHILD_DETACHED;
            } else {
                state = 0;
            }
        }

        *statePtr = state;
        statusBlock->Information = sizeof(ULONG);
        status = NO_ERROR;
        break;
    }

    case IOCTL_VIDEO_VALIDATE_CHILD_STATE_CONFIGURATION: {
        PVIDEO_CHILD_STATE_CONFIGURATION config = (PVIDEO_CHILD_STATE_CONFIGURATION)RequestPacket->InputBuffer;
        if (config && RequestPacket->InputBufferLength >= sizeof(ULONG)) {
            LogStr("NVX [DUALVIEW]: VALIDATE_CHILD_STATE count=");
            LogHex(config->Count);
            LogStr(" IsSec=");
            LogHex(devExt->IsSecondaryDisplay);
            LogStr("\n");
        }
        if (RequestPacket->OutputBuffer && RequestPacket->OutputBufferLength >= sizeof(ULONG)) {
            *(PULONG)RequestPacket->OutputBuffer = 1; /* 1 = proceed with change/extend */
            statusBlock->Information = sizeof(ULONG);
        }
        status = NO_ERROR;
        break;
    }

    case IOCTL_VIDEO_SET_CHILD_STATE_CONFIGURATION: {
        PVIDEO_CHILD_STATE_CONFIGURATION config = (PVIDEO_CHILD_STATE_CONFIGURATION)RequestPacket->InputBuffer;
        PNVX_DEVICE_EXTENSION prim = devExt->IsSecondaryDisplay ?
            (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension : devExt;
        ULONG mon1 = (prim && prim->MonitorCount >= 2) ? prim->MonitorIds[1] : 0x0200;

        if (config && RequestPacket->InputBufferLength >= sizeof(ULONG)) {
            ULONG count = config->Count;
            LogStr("NVX [DUALVIEW]: SET_CHILD_STATE count=");
            LogHex(count);
            LogStr(" IsSec=");
            LogHex(devExt->IsSecondaryDisplay);
            LogStr("\n");
            for (ULONG i = 0; i < count; i++) {
                if ((PCHAR)&config->ChildStateArray[i + 1] > (PCHAR)RequestPacket->InputBuffer + RequestPacket->InputBufferLength)
                    break;
                ULONG cid = config->ChildStateArray[i].Id;
                ULONG cstate = config->ChildStateArray[i].State;
                LogStr("NVX [DUALVIEW]: child id=");
                LogHex(cid);
                LogStr(" state=");
                LogHex(cstate);
                LogStr("\n");
                if (cid == 2 || cid == mon1) {
                    BOOLEAN attached = ((cstate & VIDEO_CHILD_ACTIVE) != 0);
                    devExt->DualViewAttached = attached;
                    if (devExt->SecondaryDeviceExtension) {
                        ((PNVX_DEVICE_EXTENSION)devExt->SecondaryDeviceExtension)->DualViewAttached = attached;
                    }
                    if (prim && prim->SecondaryDeviceExtension) {
                        ((PNVX_DEVICE_EXTENSION)prim->SecondaryDeviceExtension)->DualViewAttached = attached;
                    }
                }
            }
        }
        statusBlock->Information = 0;
        status = NO_ERROR;
        break;
    }

    case IOCTL_VIDEO_RESET_DEVICE:
    case IOCTL_VIDEO_SET_COLOR_REGISTERS:
        status = NO_ERROR;
        break;

    case IOCTL_NVX_ACCEL_COPY: {
        PNVX_ACCEL_COPY_PARAMS params = (PNVX_ACCEL_COPY_PARAMS)RequestPacket->InputBuffer;
        PNVX_DEVICE_EXTENSION engine=devExt->IsSecondaryDisplay?
            (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension:devExt;
        static ULONG s_copyCount = 0;
        ULONG64 base,surfaceBytes,srcOffset,dstOffset,srcSpan,dstSpan;
        if (!params || RequestPacket->InputBufferLength < sizeof(NVX_ACCEL_COPY_PARAMS)) {
            status = ERROR_INVALID_PARAMETER;
            break;
        }
        if (!engine || !engine->GspCopyVerified || engine->GspChannelFaulted ||
            !params->BytesPerLine || params->BytesPerLine>0x01000000UL ||
            !params->LineCount || !params->SrcPitch || !params->DstPitch ||
            params->SrcPitch<params->BytesPerLine || params->DstPitch<params->BytesPerLine ||
            (params->BytesPerLine&3) || (params->SrcPitch&3) || (params->DstPitch&3) ||
            devExt->CurrentModeIndex>=devExt->NumModes) {
            status = ERROR_NOT_SUPPORTED;
            break;
        }
        base=engine->GspVramGpuVa+(devExt->IsSecondaryDisplay?0x2000000ULL:0ULL);
        surfaceBytes=(ULONG64)devExt->Modes[devExt->CurrentModeIndex].ScreenStride*
            devExt->Modes[devExt->CurrentModeIndex].VisScreenHeight;
        srcSpan=(ULONG64)(params->LineCount-1)*params->SrcPitch+params->BytesPerLine;
        dstSpan=(ULONG64)(params->LineCount-1)*params->DstPitch+params->BytesPerLine;
        srcOffset=(params->SrcGpuVa>=base)?params->SrcGpuVa-base:surfaceBytes+1;
        dstOffset=(params->DstGpuVa>=base)?params->DstGpuVa-base:surfaceBytes+1;
        if (params->LineCount>0x10000UL || srcSpan>0x01000000ULL ||
            dstSpan>0x01000000ULL || params->SrcGpuVa<base || params->DstGpuVa<base ||
            srcOffset>surfaceBytes || dstOffset>surfaceBytes ||
            srcSpan>surfaceBytes-srcOffset || dstSpan>surfaceBytes-dstOffset ||
            ((params->SrcGpuVa-base)&3) || ((params->DstGpuVa-base)&3)) {
            status=ERROR_INVALID_PARAMETER;
            break;
        }
        /* The copy path rejects overlapping spans; CE overlap semantics are
           not assumed for a multi-row DMA. */
        if (srcOffset<dstOffset+dstSpan && dstOffset<srcOffset+srcSpan) {
            status=ERROR_INVALID_PARAMETER;
            break;
        }
        if (NvxGspSubmitCopy2D(engine,params->DstGpuVa,params->SrcGpuVa,
                               params->SrcPitch,params->DstPitch,
                               params->BytesPerLine,params->LineCount)) {
            status = NO_ERROR;
            statusBlock->Information = sizeof(NVX_ACCEL_COPY_PARAMS);
            s_copyCount++;
            if ((s_copyCount <= 5) || (s_copyCount % 10000 == 0)) {
                LogStr("NVX [ACCEL]: GPU Copy #"); LogHex(s_copyCount);
                LogStr(" srcOffset=");LogHex(srcOffset);
                LogStr(" dstOffset=");LogHex(dstOffset);
                LogStr(" bytesPerLine=");LogHex(params->BytesPerLine);
                LogStr(" lines=");LogHex(params->LineCount);LogStr("\n");
            }
        } else {
            status = ERROR_DEVICE_NOT_READY;
        }
        break;
    }

    case IOCTL_NVX_ACCEL_FILL: {
        PNVX_ACCEL_FILL_PARAMS params = (PNVX_ACCEL_FILL_PARAMS)RequestPacket->InputBuffer;
        PNVX_DEVICE_EXTENSION engine=devExt->IsSecondaryDisplay?
            (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension:devExt;
        static ULONG s_fillCount = 0;
        ULONG64 base,surfaceBytes,offset;
        if (!params || RequestPacket->InputBufferLength != sizeof(NVX_ACCEL_FILL_PARAMS)) {
            status = ERROR_INVALID_PARAMETER;
            break;
        }
        if (!engine || !engine->GspCopyVerified || engine->GspChannelFaulted ||
            devExt->CurrentModeIndex>=devExt->NumModes || !params->Bytes ||
            params->Bytes>0x01000000UL || (params->Bytes&3)) {
            status = ERROR_NOT_SUPPORTED;
            break;
        }
        base=engine->GspVramGpuVa+(devExt->IsSecondaryDisplay?0x2000000ULL:0ULL);
        surfaceBytes=(ULONG64)devExt->Modes[devExt->CurrentModeIndex].ScreenStride*
            devExt->Modes[devExt->CurrentModeIndex].VisScreenHeight;
        if(params->DstGpuVa<base || params->DstGpuVa-base>surfaceBytes ||
           params->Bytes>surfaceBytes-(params->DstGpuVa-base) ||
           ((params->DstGpuVa-base)&3)) {
            status=ERROR_INVALID_PARAMETER;
            break;
        }
        offset=params->DstGpuVa-base;
        if (NvxGspSubmitFill(engine, params->DstGpuVa, params->Color, params->Bytes)) {
            status = NO_ERROR;
            statusBlock->Information = sizeof(NVX_ACCEL_FILL_PARAMS);
            s_fillCount++;
            if ((s_fillCount <= 5) || (s_fillCount % 10000 == 0)) {
                LogStr("NVX [2D]: GPU solid fill #"); LogHex(s_fillCount);
                LogStr(" offset=");LogHex(offset);
                LogStr(" color "); LogHex(params->Color);
                LogStr(" bytes "); LogHex(params->Bytes); LogStr("\n");
            }
        } else {
            status = ERROR_DEVICE_NOT_READY;
        }
        break;
    }

    case IOCTL_NVX_SCANOUT_GPU_VA: {
        PNVX_DEVICE_EXTENSION engine=devExt->IsSecondaryDisplay?
            (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension:devExt;
        if(!RequestPacket->OutputBuffer || RequestPacket->OutputBufferLength<sizeof(ULONG64)) {
            status=ERROR_INSUFFICIENT_BUFFER;break;
        }
        if(!engine || !engine->GspCopyVerified || engine->GspChannelFaulted) {
            LogStr("NVX [SCANOUT-VA]: rejected; engine=");LogHex(engine!=NULL);
            if(engine) {
                LogStr(" initialized=");LogHex(engine->GspInitialized);
                LogStr(" boot_verified=");LogHex(engine->GspBootVerified);
                LogStr(" copy_verified=");LogHex(engine->GspCopyVerified);
                LogStr(" channel_faulted=");LogHex(engine->GspChannelFaulted);
                LogStr(" fwsec_frts=");LogHex(engine->FwsecFrtsReady);
                LogStr(" fb_layout=");LogHex(engine->GspFbLayoutValid);
                LogStr(" scanout_mapped=");LogHex(engine->GspScanoutMapped);
                LogStr(" gpu_va=");LogHex(engine->GspVramGpuVa);
            }
            LogStr(" secondary=");LogHex(devExt->IsSecondaryDisplay);LogStr("\n");
            status=ERROR_DEVICE_NOT_READY;break;
        }
        *(PULONG64)RequestPacket->OutputBuffer=engine->GspVramGpuVa+
            (devExt->IsSecondaryDisplay?0x2000000ULL:0ULL);
        statusBlock->Information=sizeof(ULONG64);status=NO_ERROR;
        LogStr("NVX [SCANOUT-VA]: secondary=");LogHex(devExt->IsSecondaryDisplay);
        LogStr(" gpuVa=");LogHex(*(PULONG64)RequestPacket->OutputBuffer);LogStr("\n");
        break;
    }

    case IOCTL_NVX_DIAGNOSTIC_TRACE: {
        const ULONG *trace=(const ULONG *)RequestPacket->InputBuffer;
        if(!trace || RequestPacket->InputBufferLength<4*sizeof(ULONG)) {
            status=ERROR_INVALID_PARAMETER;
            break;
        }
        LogStr("NVX [GDI-TRACE]: stage=");LogHex(trace[0]);
        LogStr(" v0=");LogHex(trace[1]);
        LogStr(" v1=");LogHex(trace[2]);
        LogStr(" v2=");LogHex(trace[3]);LogStr("\n");
        status=NO_ERROR;
        break;
    }

    case IOCTL_NVX_DIAGNOSTIC_STATE: {
        PNVX_DIAGNOSTIC_STATE state=(PNVX_DIAGNOSTIC_STATE)RequestPacket->OutputBuffer;
        PNVX_DEVICE_EXTENSION engine=devExt->IsSecondaryDisplay?
            (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension:devExt;
        if(!state || RequestPacket->OutputBufferLength<sizeof(*state)) {
            status=ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        VideoPortZeroMemory(state,sizeof(*state));
        if(devExt->IsSecondaryDisplay) state->Flags|=0x00000001UL;
        if(engine) {
            state->Flags|=0x00000002UL;
            if(engine->GspInitialized) state->Flags|=0x00000004UL;
            if(engine->GspBootVerified) state->Flags|=0x00000008UL;
            if(engine->GspCopyVerified) state->Flags|=0x00000010UL;
            if(engine->GspChannelFaulted) state->Flags|=0x00000020UL;
            if(engine->FwsecFrtsReady) state->Flags|=0x00000040UL;
            if(engine->GspFbLayoutValid) state->Flags|=0x00000080UL;
            if(engine->GspScanoutMapped) state->Flags|=0x00000100UL;
            state->MonitorCount=engine->MonitorCount;
            state->GspVramGpuVa=engine->GspVramGpuVa;
        }
        state->NumModes=devExt->NumModes;
        state->CurrentModeIndex=devExt->CurrentModeIndex;
        statusBlock->Information=sizeof(*state);
        status=NO_ERROR;
        break;
    }

    case IOCTL_NVX_3D_QUERY_CAPS: {
        PNVX_DEVICE_EXTENSION engine=devExt->IsSecondaryDisplay?
            (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension:devExt;
        NVX_3D_QUERY_CAPS *caps=(NVX_3D_QUERY_CAPS *)RequestPacket->OutputBuffer;
        ULONG64 heapStart,heapEnd;
        if(!__sync_lock_test_and_set(&g_Nvx3dProcessContextLogged,1)) {
            LogStr("NVX [3D-CLIENT-CONTEXT]: HwStartIO process id=");
            LogHex((ULONG64)(ULONG_PTR)PsGetCurrentProcessId());
            LogStr("\n");
        }
        if(!caps||RequestPacket->OutputBufferLength<sizeof(*caps)) {
            status=ERROR_INSUFFICIENT_BUFFER;break;
        }
        Nvx3dBuildCaps(caps,engine&&engine->GspGraphicsChannelReady,
            engine&&engine->GspGraphicsCommandVerified,
            engine&&engine->GspGraphicsChannelFaulted,
            engine&&engine->GspMmuFaultQueued,
            engine&&engine->GspGraphicsDataWriteVerified,
            engine&&engine->GspGraphicsPixelClearVerified,
            engine&&engine->GspCopyVerified&&
                Nvx3dHeapBounds(engine,&heapStart,&heapEnd),
            engine&&engine->GspGraphicsChannelReady&&
                engine->GspGraphicsCommandVerified&&
                engine->GspGraphicsPixelClearVerified);
        if(engine&&engine->GspGraphicsM2mfObjectHandle)
            caps->Features|=NVX_3D_FEATURE_NVC0_M2MF_OBJECT;
        if(engine&&engine->GspGraphicsCopyObjectHandle)
            caps->Features|=NVX_3D_FEATURE_NVC0_COPY_OBJECT;
        if(engine&&engine->GspGraphics2dObjectHandle)
            caps->Features|=NVX_3D_FEATURE_NVC0_2D_OBJECT;
        if(engine) {
            caps->DeviceId=engine->DeviceId;
            caps->Chipset=Nvx3dChipsetFromDeviceId(engine->DeviceId);
            caps->PciLocation=Nvx3dPackPciLocation(engine->PciBusNumber,
                                                   engine->PciSlot);
            caps->VramApertureBytes=engine->GspVramMapBytes;
            caps->GartBytes=engine->GspGartReady?NVX_GART_TOTAL_BYTES:0;
        }
        if(engine&&!(caps->Features&NVX_3D_FEATURE_GR_CHANNEL)) {
            LogStr("NVX [3D-CAPS]: channel_ready=");
            LogHex(engine->GspGraphicsChannelReady);
            LogStr(" command_verified=");LogHex(engine->GspGraphicsCommandVerified);
            LogStr(" channel_faulted=");LogHex(engine->GspGraphicsChannelFaulted);
            LogStr(" mmu_faulted=");LogHex(engine->GspMmuFaultQueued);
            LogStr(" copy_verified=");LogHex(engine->GspCopyVerified);
            LogStr(" clear_verified=");LogHex(engine->GspGraphicsPixelClearVerified);
            LogStr("\n");
        }
        if(engine&&engine->GspGraphicsTriangleVerified)
            caps->Features|=NVX_3D_FEATURE_TEST_TRIANGLE;
        if(engine&&engine->GspGraphicsClientTriangleVerified)
            caps->Features|=NVX_3D_FEATURE_CLIENT_TRIANGLE;
        RequestPacket->StatusBlock->Information=sizeof(*caps);
        status=NO_ERROR;
        break;
    }

    case IOCTL_NVX_3D_ALLOC_BUFFER: {
        PNVX_DEVICE_EXTENSION engine=devExt->IsSecondaryDisplay?
            (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension:devExt;
        NVX_3D_ALLOC_BUFFER request;
        if(!RequestPacket->InputBuffer||
           RequestPacket->InputBufferLength!=sizeof(request)||
           !RequestPacket->OutputBuffer||
           RequestPacket->OutputBufferLength<sizeof(request)) {
            status=ERROR_INVALID_PARAMETER;break;
        }
        VideoPortMoveMemory(&request,RequestPacket->InputBuffer,sizeof(request));
        if(!Nvx3dValidateAlloc(&request)) {status=ERROR_INVALID_PARAMETER;break;}
        if(!Nvx3dAllocateBuffer(engine,&request)) {
            status=engine&&engine->GspCopyVerified?ERROR_NOT_ENOUGH_MEMORY:
                                                    ERROR_NOT_SUPPORTED;
            LogStr("NVX [3D-ALLOC]: failed usage=");LogHex(request.Usage);
            LogStr(" bytes=");LogHex(request.Bytes);
            LogStr(" width=");LogHex(request.Width);
            LogStr(" height=");LogHex(request.Height);
            LogStr(" copy_verified=");LogHex(engine&&engine->GspCopyVerified);
            LogStr(" channel_faulted=");LogHex(engine&&engine->GspChannelFaulted);
            LogStr("\n");
            break;
        }
        VideoPortMoveMemory(RequestPacket->OutputBuffer,&request,sizeof(request));
        statusBlock->Information=sizeof(request);status=NO_ERROR;
        break;
    }

    case IOCTL_NVX_3D_FREE_BUFFER: {
        PNVX_DEVICE_EXTENSION engine=devExt->IsSecondaryDisplay?
            (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension:devExt;
        NVX_3D_FREE_BUFFER request;
        if(!RequestPacket->InputBuffer||
           RequestPacket->InputBufferLength!=sizeof(request)) {
            status=ERROR_INVALID_PARAMETER;break;
        }
        VideoPortMoveMemory(&request,RequestPacket->InputBuffer,sizeof(request));
        if(!Nvx3dValidateFree(&request)) {status=ERROR_INVALID_PARAMETER;break;}
        if(Nvx3dFreeBuffer(engine,request.BufferId,request.ClientToken)) status=NO_ERROR;
        else {
            LogStr("NVX [3D-FREE]: failed buffer id=");LogHex(request.BufferId);
            LogStr("\n");status=ERROR_NOT_FOUND;
        }
        break;
    }

    case IOCTL_NVX_3D_UPLOAD_BUFFER: {
        PNVX_DEVICE_EXTENSION engine=devExt->IsSecondaryDisplay?
            (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension:devExt;
        NVX_3D_UPLOAD_BUFFER upload;
        NVX_3D_USER_BUFFER_ENTRY saved;
        PNVX_3D_USER_BUFFER_ENTRY entry;
        ULONG headerBytes=sizeof(upload);
        ULONG inputBytes=RequestPacket->InputBufferLength;
        if(!RequestPacket->InputBuffer||inputBytes<headerBytes||
           !engine||!Nvx3dLockBuffers()) {
            status=ERROR_INVALID_PARAMETER;break;
        }
        VideoPortMoveMemory(&upload,RequestPacket->InputBuffer,headerBytes);
        if(upload.Size!=headerBytes||upload.Version!=NVX_3D_ABI_VERSION||
           !upload.BufferId||!upload.Bytes||
           upload.Bytes>NVX_3D_MAX_UPLOAD_BYTES||
           inputBytes!=headerBytes+upload.Bytes) {
            Nvx3dUnlockBuffers();status=ERROR_INVALID_PARAMETER;break;
        }
        entry=Nvx3dFindBuffer(engine,upload.BufferId);
        if(!entry||entry->Busy||entry->ClientToken!=upload.ClientToken||
           !Nvx3dClientSessionActiveLocked(engine,upload.ClientToken)||
           !Nvx3dValidateUpload(&upload,inputBytes,entry->Bytes)) {
            Nvx3dUnlockBuffers();status=ERROR_INVALID_PARAMETER;break;
        }
        entry->Busy=TRUE;saved=*entry;
        Nvx3dUnlockBuffers();
        if(NvxGspUpload(engine,saved.GpuVa+upload.Offset,
                (PUCHAR)RequestPacket->InputBuffer+headerBytes,upload.Bytes)) {
            status=NO_ERROR;
            statusBlock->Information=inputBytes;
        } else {
            status=ERROR_DEVICE_NOT_READY;
        }
        if(Nvx3dLockBuffers()) {
            entry=Nvx3dFindBuffer(engine,saved.Id);
            if(entry) entry->Busy=FALSE;
            Nvx3dUnlockBuffers();
        }
        break;
    }

    case IOCTL_NVX_3D_DOWNLOAD_BUFFER: {
        PNVX_DEVICE_EXTENSION engine=devExt->IsSecondaryDisplay?
            (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension:devExt;
        NVX_3D_DOWNLOAD_BUFFER download;
        NVX_3D_USER_BUFFER_ENTRY saved;
        PNVX_3D_USER_BUFFER_ENTRY entry;
        ULONG headerBytes=sizeof(download);
        if(!RequestPacket->InputBuffer||
           RequestPacket->InputBufferLength!=headerBytes||
           !RequestPacket->OutputBuffer||!engine||!Nvx3dLockBuffers()) {
            status=ERROR_INVALID_PARAMETER;break;
        }
        VideoPortMoveMemory(&download,RequestPacket->InputBuffer,headerBytes);
        entry=Nvx3dFindBuffer(engine,download.BufferId);
        if(!entry||entry->Busy||entry->ClientToken!=download.ClientToken||
           !Nvx3dClientSessionActiveLocked(engine,download.ClientToken)||
           !Nvx3dValidateDownload(&download,headerBytes,
                RequestPacket->OutputBufferLength,entry->Bytes)) {
            Nvx3dUnlockBuffers();status=ERROR_INVALID_PARAMETER;break;
        }
        entry->Busy=TRUE;saved=*entry;
        Nvx3dUnlockBuffers();
        if(NvxGspReadUserBuffer(engine,saved.GpuVa+download.Offset,
                RequestPacket->OutputBuffer,download.Bytes)) {
            status=NO_ERROR;
            statusBlock->Information=download.Bytes;
        } else {
            status=ERROR_DEVICE_NOT_READY;
        }
        if(Nvx3dLockBuffers()) {
            entry=Nvx3dFindBuffer(engine,saved.Id);
            if(entry) entry->Busy=FALSE;
            Nvx3dUnlockBuffers();
        }
        break;
    }

    case IOCTL_NVX_3D_READBACK_BUFFER: {
        PNVX_DEVICE_EXTENSION engine=devExt->IsSecondaryDisplay?
            (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension:devExt;
        NVX_3D_READBACK request;
        NVX_3D_USER_BUFFER_ENTRY saved;
        PNVX_3D_USER_BUFFER_ENTRY entry;
        if(!RequestPacket->InputBuffer||
           RequestPacket->InputBufferLength!=sizeof(request)||
           !RequestPacket->OutputBuffer||
           RequestPacket->OutputBufferLength<NVX_3D_READBACK_BYTES||
           !engine||!Nvx3dLockBuffers()) {
            status=ERROR_INVALID_PARAMETER;break;
        }
        VideoPortMoveMemory(&request,RequestPacket->InputBuffer,sizeof(request));
        entry=Nvx3dFindBuffer(engine,request.BufferId);
        if(!entry||entry->Busy||
           !Nvx3dValidateReadback(&request,
               RequestPacket->OutputBufferLength,entry->Bytes)) {
            Nvx3dUnlockBuffers();status=ERROR_INVALID_PARAMETER;break;
        }
        if(entry->Usage&NVX_3D_USAGE_COLOR_TARGET) {
            if(request.Reserved[0]>=entry->Width||
               request.Reserved[1]>=entry->Height) {
                Nvx3dUnlockBuffers();status=ERROR_INVALID_PARAMETER;break;
            }
        } else if(request.Reserved[0]||request.Reserved[1]) {
            Nvx3dUnlockBuffers();status=ERROR_INVALID_PARAMETER;break;
        }
        entry->Busy=TRUE;saved=*entry;
        Nvx3dUnlockBuffers();
        if(((saved.Usage&NVX_3D_USAGE_COLOR_TARGET)?
            NvxGspReadUserColorTargetTile(engine,saved.GpuVa,saved.Width,
                saved.Height,saved.TileMode,request.Reserved[0],
                request.Reserved[1],(PULONG)RequestPacket->OutputBuffer,
                NVX_3D_READBACK_PIXELS):
            NvxGspReadUserSurface(engine,saved.GpuVa,
                (PULONG)RequestPacket->OutputBuffer,NVX_3D_READBACK_PIXELS))) {
            status=NO_ERROR;
            statusBlock->Information=NVX_3D_READBACK_BYTES;
        } else {
            status=ERROR_DEVICE_NOT_READY;
        }
        if(Nvx3dLockBuffers()) {
            entry=Nvx3dFindBuffer(engine,saved.Id);
            if(entry) entry->Busy=FALSE;
            Nvx3dUnlockBuffers();
        }
        break;
    }

    case IOCTL_NVX_3D_NVC0_CLEAR:
    case IOCTL_NVX_3D_NVC0_NATIVE_CLEAR: {
        PNVX_DEVICE_EXTENSION engine=devExt->IsSecondaryDisplay?
            (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension:devExt;
        NVX_3D_NVC0_CLEAR request;
        NVX_3D_USER_BUFFER_ENTRY saved;
        PNVX_3D_USER_BUFFER_ENTRY entry;
        ULONG64 fenceValue;
        ULONG color;
        BOOLEAN nativeClear=(RequestPacket->IoControlCode==
                             IOCTL_NVX_3D_NVC0_NATIVE_CLEAR);
        uint32_t parsedColor;
        if(!RequestPacket->InputBuffer||
           RequestPacket->InputBufferLength!=sizeof(request)||
           !RequestPacket->OutputBuffer||
           RequestPacket->OutputBufferLength<sizeof(request)) {
            status=ERROR_INVALID_PARAMETER;break;
        }
        VideoPortMoveMemory(&request,RequestPacket->InputBuffer,sizeof(request));
        if(!engine||!Nvx3dLockBuffers()) {
            status=ERROR_DEVICE_NOT_READY;break;
        }
        entry=Nvx3dFindBuffer(engine,request.BufferId);
        if(!entry||entry->Busy||
           entry->ClientToken!=request.ClientToken||
           !Nvx3dClientSessionActiveLocked(engine,request.ClientToken)||
           !(entry->Usage&NVX_3D_USAGE_COLOR_TARGET)||
           (entry->Usage&NVX_3D_USAGE_DEPTH_TARGET)||
           !Nvx3dValidateNvc0Clear(&request,sizeof(request),
                entry->ClientToken,entry->GpuVa,entry->Bytes,entry->Width,entry->Height,
                entry->PitchBytes,entry->TileMode,&parsedColor)) {
            Nvx3dUnlockBuffers();status=ERROR_INVALID_PARAMETER;break;
        }
        color=(ULONG)parsedColor;
        entry->Busy=TRUE;saved=*entry;
        fenceValue=engine->Nvx3dLastFence+1U;
        if(!fenceValue||fenceValue>0xffffffffULL) {
            entry->Busy=FALSE;Nvx3dUnlockBuffers();
            status=ERROR_DEVICE_NOT_READY;break;
        }
        engine->Nvx3dLastFence=fenceValue;
        Nvx3dUnlockBuffers();

        if((nativeClear&&NvxGspSubmitNvc0NativeClear(engine,request.Dwords,
                                                     fenceValue))||
           (!nativeClear&&NvxGspSubmitNvc0ColorTargetClear(engine,saved.GpuVa,
                saved.Width,saved.Height,saved.TileMode,color,fenceValue))) {
            request.FenceValue=fenceValue;
            VideoPortMoveMemory(RequestPacket->OutputBuffer,&request,
                                sizeof(request));
            statusBlock->Information=sizeof(request);status=NO_ERROR;
            engine->GspGraphicsUserSubmitReady=TRUE;
        } else {
            status=ERROR_DEVICE_NOT_READY;
        }
        if(Nvx3dLockBuffers()) {
            entry=Nvx3dFindBuffer(engine,saved.Id);
            if(entry) entry->Busy=FALSE;
            Nvx3dUnlockBuffers();
        }
        break;
    }

    case IOCTL_NVX_3D_VALIDATE_BATCH: {
        PNVX_DEVICE_EXTENSION engine=devExt->IsSecondaryDisplay?
            (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension:devExt;
        NVX_3D_BATCH_HEADER header;
        PVOID copiedInput=NULL;
        NVX_3D_ALLOC_BUFFER *ownedAllocations=NULL;
        ULONG ownedCount=0,i;
        uint32_t requiredBytes;
        if(!RequestPacket->InputBuffer||
           RequestPacket->InputBufferLength<sizeof(header)||
           RequestPacket->InputBufferLength>NVX_3D_MAX_BATCH_BYTES||
           RequestPacket->OutputBufferLength) {
            status=ERROR_INVALID_PARAMETER;break;
        }
        VideoPortMoveMemory(&header,RequestPacket->InputBuffer,sizeof(header));
        if(!Nvx3dBatchRequiredBytes(&header,&requiredBytes)||
           requiredBytes!=RequestPacket->InputBufferLength||
           !header.ClientToken) {
            status=ERROR_INVALID_PARAMETER;break;
        }

        /* Parse only a stable kernel-owned copy of the METHOD_BUFFERED input. */
        copiedInput=VideoPortAllocatePool(devExt,VpNonPagedPool,
                         requiredBytes,0x4258564eUL);
        ownedAllocations=(NVX_3D_ALLOC_BUFFER *)VideoPortAllocatePool(
                         devExt,VpNonPagedPool,
                         NVX_3D_MAX_BATCH_BUFFERS*sizeof(*ownedAllocations),
                         0x4158564eUL);
        if(!copiedInput||!ownedAllocations) {
            status=ERROR_NOT_ENOUGH_MEMORY;goto validate_batch_done;
        }
        VideoPortMoveMemory(copiedInput,RequestPacket->InputBuffer,
                            requiredBytes);

        if(!engine||!Nvx3dLockBuffers()) {
            status=ERROR_DEVICE_NOT_READY;goto validate_batch_done;
        }
        if(!Nvx3dClientSessionActiveLocked(engine,header.ClientToken)) {
            Nvx3dUnlockBuffers();
            status=ERROR_INVALID_PARAMETER;goto validate_batch_done;
        }
        for(i=0;i<NVX_3D_USER_BUFFER_SLOTS;i++) {
            NVX_3D_USER_BUFFER_ENTRY *entry=&engine->Nvx3dBuffers[i];
            NVX_3D_ALLOC_BUFFER *allocation;
            if(!entry->Id||entry->Busy||
               entry->ClientToken!=header.ClientToken) continue;
            if(ownedCount>=NVX_3D_MAX_BATCH_BUFFERS) break;
            allocation=&ownedAllocations[ownedCount++];
            VideoPortZeroMemory(allocation,sizeof(*allocation));
            allocation->Size=sizeof(*allocation);
            allocation->Version=NVX_3D_ABI_VERSION;
            allocation->Usage=entry->Usage;
            allocation->Bytes=entry->Bytes;
            allocation->BufferId=entry->Id;
            allocation->GpuVa=entry->GpuVa;
            allocation->Width=entry->Width;
            allocation->Height=entry->Height;
            allocation->PitchBytes=entry->PitchBytes;
            allocation->TileMode=entry->TileMode;
            allocation->ClientToken=entry->ClientToken;
        }
        Nvx3dUnlockBuffers();

        if(!Nvx3dValidateNvc0Batch(copiedInput,requiredBytes,
                ownedAllocations,ownedCount,header.ClientToken)) {
            status=ERROR_INVALID_PARAMETER;
            goto validate_batch_done;
        }
        /* Validation-only: no method allowlist or GPU submission is implied. */
        status=NO_ERROR;

validate_batch_done:
        if(ownedAllocations) VideoPortFreePool(devExt,ownedAllocations);
        if(copiedInput) VideoPortFreePool(devExt,copiedInput);
        break;
    }

    case IOCTL_NVX_3D_SUBMIT: {
        PNVX_DEVICE_EXTENSION engine=devExt->IsSecondaryDisplay?
            (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension:devExt;
        NVX_3D_SUBMIT request;
        NVX_3D_USER_BUFFER_ENTRY saved={0},vertexSaved={0},depthSaved={0};
        PNVX_3D_USER_BUFFER_ENTRY entry,vertexEntry=NULL,depthEntry=NULL;
        ULONG64 fenceValue,vertexBufferId,depthBufferId;
        if(!RequestPacket->InputBuffer||
           RequestPacket->InputBufferLength!=sizeof(request)||
           !RequestPacket->OutputBuffer||
           RequestPacket->OutputBufferLength<sizeof(request)) {
            status=ERROR_INVALID_PARAMETER;break;
        }
        VideoPortMoveMemory(&request,RequestPacket->InputBuffer,sizeof(request));
        if(!Nvx3dValidateSubmit(&request,sizeof(request),NVX_3D_MAX_BUFFER_BYTES)||
           !engine||!Nvx3dLockBuffers()) {
            if(Nvx3dIsDynamicDepthDraw(request.Operation)) {
                LogStr("NVX [3D-DYNAMIC]: request/lock rejected op=");
                LogHex(request.Operation);LogStr(" width=");LogHex(request.Width);
                LogStr(" height=");LogHex(request.Height);
                LogStr(" colorId=");LogHex(request.BufferId);
                LogStr(" vertexCount=");LogHex(request.Reserved[0]);
                LogStr(" vertexId=");LogHex(Nvx3dSubmitVertexBufferId(&request));
                LogStr(" tileMode=");LogHex(request.Reserved[3]);
                LogStr(" depthId=");
                LogHex(((ULONG64)request.Reserved[5]<<32)|request.Reserved[4]);
                LogStr(" inputBytes=");LogHex(RequestPacket->InputBufferLength);
                LogStr(" validate=");LogHex(Nvx3dValidateSubmit(&request,
                    sizeof(request),NVX_3D_MAX_BUFFER_BYTES));LogStr("\n");
            }
            if(request.Operation==NVX_3D_SUBMIT_CLEAR_DEPTH||
               request.Operation==NVX_3D_SUBMIT_DRAW_TRIANGLE_DEPTH) {
                LogStr("NVX [3D-DEPTH]: submit structure/lock rejected.");
                LogStr(" op=");LogHex(request.Operation);
                LogStr(" size=");LogHex(request.Size);
                LogStr(" version=");LogHex(request.Version);
                LogStr(" target=");LogHex(request.BufferId);
                LogStr(" dimensions=");LogHex(request.Width);LogStr("x");
                LogHex(request.Height);LogStr(" bytes=");
                LogHex(RequestPacket->InputBufferLength);LogStr("\n");
            }
            status=ERROR_INVALID_PARAMETER;break;
        }
        if(!Nvx3dClientSessionActiveLocked(engine,request.ClientToken)) {
            Nvx3dUnlockBuffers();status=ERROR_INVALID_PARAMETER;break;
        }
        if(request.ClientToken&&request.RequestToken==
              engine->Nvx3dLastSubmitToken&&
           request.ClientToken==engine->Nvx3dLastSubmitClientToken) {
            NVX_3D_SUBMIT replay;
            VideoPortMoveMemory(&replay,engine->Nvx3dLastSubmitReply,
                                sizeof(replay));
            Nvx3dUnlockBuffers();
            VideoPortMoveMemory(RequestPacket->OutputBuffer,&replay,
                                sizeof(replay));
            statusBlock->Information=sizeof(replay);status=NO_ERROR;
            LogStr("NVX [3D-SUBMIT]: replay token=");
            LogHex(request.ClientToken);LogStr(" fence=");
            LogHex(replay.FenceValue);LogStr("\n");
            break;
        }
        vertexBufferId=Nvx3dSubmitVertexBufferId(&request);
        depthBufferId=Nvx3dIsDynamicDepthDraw(request.Operation)?
            (((ULONG64)request.Reserved[5]<<32)|request.Reserved[4]):0;
        entry=Nvx3dFindBuffer(engine,request.BufferId);
        if(!entry||entry->Busy||entry->Bytes<
           Nvx3dRequiredSubmitBytes(request.Operation)) {
            if(Nvx3dIsDynamicDepthDraw(request.Operation)) {
                LogStr("NVX [3D-DYNAMIC]: color buffer rejected id=");
                LogHex(request.BufferId);LogStr(" found=");LogHex(entry!=NULL);
                LogStr(" busy=");LogHex(entry?entry->Busy:0);
                LogStr(" bytes=");LogHex(entry?entry->Bytes:0);
                LogStr(" usage=");LogHex(entry?entry->Usage:0);LogStr("\n");
            }
            if(request.Operation==NVX_3D_SUBMIT_CLEAR_DEPTH||
               request.Operation==NVX_3D_SUBMIT_DRAW_TRIANGLE_DEPTH) {
                LogStr("NVX [3D-DEPTH]: target buffer rejected.");
                LogStr(" op=");LogHex(request.Operation);
                LogStr(" id=");LogHex(request.BufferId);
                LogStr(" found=");LogHex(entry!=NULL);
                LogStr(" busy=");LogHex(entry?entry->Busy:0);
                LogStr(" bytes=");LogHex(entry?entry->Bytes:0);
                LogStr(" required=");
                LogHex(Nvx3dRequiredSubmitBytes(request.Operation));LogStr("\n");
            }
            Nvx3dUnlockBuffers();status=ERROR_INVALID_PARAMETER;break;
        }
        if(request.Operation==NVX_3D_SUBMIT_CLEAR_COLOR_TARGET &&
           (!(entry->Usage&NVX_3D_USAGE_COLOR_TARGET) ||
            request.Width!=entry->Width || request.Height!=entry->Height ||
            entry->TileMode>0x70U || !entry->PitchBytes)) {
            Nvx3dUnlockBuffers();status=ERROR_INVALID_PARAMETER;break;
        }
        if(request.Operation==NVX_3D_SUBMIT_CLEAR_COLOR_TARGET_DEPTH &&
           (!(entry->Usage&NVX_3D_USAGE_DEPTH_TARGET) ||
            !(entry->Usage&NVX_3D_USAGE_DEPTH_ZF32) ||
            request.Width!=entry->Width || request.Height!=entry->Height ||
            entry->Bytes<=NVX_3D_DEPTH_TARGET_OFFSET)) {
            Nvx3dUnlockBuffers();status=ERROR_INVALID_PARAMETER;break;
        }
        if((request.Operation==NVX_3D_SUBMIT_DRAW_COLOR_TARGET ||
            request.Operation==NVX_3D_SUBMIT_VERIFY_COLOR_TARGET_DRAW ||
            Nvx3dIsDynamicDepthDraw(request.Operation)) &&
           (!(entry->Usage&NVX_3D_USAGE_COLOR_TARGET) ||
            request.Width!=entry->Width || request.Height!=entry->Height ||
            request.Reserved[3]!=entry->TileMode || !entry->PitchBytes)) {
            if(Nvx3dIsDynamicDepthDraw(request.Operation)) {
                LogStr("NVX [3D-DYNAMIC]: color metadata rejected id=");
                LogHex(entry->Id);LogStr(" req=");LogHex(request.Width);
                LogStr("x");LogHex(request.Height);LogStr(" tile=");
                LogHex(request.Reserved[3]);LogStr(" entry=");
                LogHex(entry->Width);LogStr("x");LogHex(entry->Height);
                LogStr(" tile=");LogHex(entry->TileMode);
                LogStr(" pitch=");LogHex(entry->PitchBytes);
                LogStr(" usage=");LogHex(entry->Usage);LogStr("\n");
            }
            Nvx3dUnlockBuffers();status=ERROR_INVALID_PARAMETER;break;
        }
        entry->Busy=TRUE;saved=*entry;
        if(request.Operation==NVX_3D_SUBMIT_DRAW_TRIANGLE||
           request.Operation==NVX_3D_SUBMIT_DRAW_TRIANGLE_PRESERVE||
           request.Operation==NVX_3D_SUBMIT_DRAW_TRIANGLE_DEPTH||
           request.Operation==NVX_3D_SUBMIT_DRAW_TRIANGLES||
           request.Operation==NVX_3D_SUBMIT_DRAW_COLOR_TARGET||
           request.Operation==NVX_3D_SUBMIT_VERIFY_COLOR_TARGET_DRAW||
           Nvx3dIsDynamicDepthDraw(request.Operation)) {
            vertexEntry=Nvx3dFindBuffer(engine,vertexBufferId);
            if(!vertexEntry||vertexEntry->Busy||vertexEntry->Bytes<
               (request.Operation==NVX_3D_SUBMIT_DRAW_TRIANGLE_DEPTH?
                    96U:request.Operation==NVX_3D_SUBMIT_DRAW_TRIANGLES?
                    request.Height*24U:
                    Nvx3dIsDynamicDepthDraw(request.Operation)?
                    request.Reserved[0]*32U:
                    (request.Operation==NVX_3D_SUBMIT_DRAW_COLOR_TARGET||
                     request.Operation==NVX_3D_SUBMIT_VERIFY_COLOR_TARGET_DRAW)?
                    request.Reserved[0]*24U:72U)) {
                if(request.Operation==NVX_3D_SUBMIT_DRAW_TRIANGLE_DEPTH) {
                    LogStr("NVX [3D-DEPTH]: vertex buffer rejected.");
                    LogStr(" id=");LogHex(vertexBufferId);
                    LogStr(" found=");LogHex(vertexEntry!=NULL);
                    LogStr(" busy=");LogHex(vertexEntry?vertexEntry->Busy:0);
                    LogStr(" bytes=");LogHex(vertexEntry?vertexEntry->Bytes:0);
                    LogStr(" needed=0x60\n");
                }
                if(Nvx3dIsDynamicDepthDraw(request.Operation)) {
                    LogStr("NVX [3D-DYNAMIC]: vertex buffer rejected id=");
                    LogHex(vertexBufferId);LogStr(" found=");
                    LogHex(vertexEntry!=NULL);LogStr(" busy=");
                    LogHex(vertexEntry?vertexEntry->Busy:0);LogStr(" bytes=");
                    LogHex(vertexEntry?vertexEntry->Bytes:0);
                    LogStr(" needed=");LogHex(request.Reserved[0]*32U);
                    LogStr("\n");
                }
                entry->Busy=FALSE;Nvx3dUnlockBuffers();
                status=ERROR_INVALID_PARAMETER;break;
            }
            vertexEntry->Busy=TRUE;vertexSaved=*vertexEntry;
        }
        if(Nvx3dIsDynamicDepthDraw(request.Operation)) {
            depthEntry=Nvx3dFindBuffer(engine,depthBufferId);
            if(!depthEntry||depthEntry->Busy||
               !(depthEntry->Usage&NVX_3D_USAGE_DEPTH_TARGET)||
               !(depthEntry->Usage&NVX_3D_USAGE_DEPTH_ZF32)||
               depthEntry->Width!=request.Width||
               depthEntry->Height!=request.Height||
               depthEntry->Bytes<=NVX_3D_DEPTH_TARGET_OFFSET) {
                LogStr("NVX [3D-DYNAMIC]: depth buffer rejected id=");
                LogHex(depthBufferId);LogStr(" found=");
                LogHex(depthEntry!=NULL);LogStr(" busy=");
                LogHex(depthEntry?depthEntry->Busy:0);LogStr(" usage=");
                LogHex(depthEntry?depthEntry->Usage:0);LogStr(" dimensions=");
                LogHex(depthEntry?depthEntry->Width:0);LogStr("x");
                LogHex(depthEntry?depthEntry->Height:0);LogStr(" bytes=");
                LogHex(depthEntry?depthEntry->Bytes:0);LogStr(" pitch=");
                LogHex(depthEntry?depthEntry->PitchBytes:0);LogStr("\n");
                entry->Busy=FALSE;
                if(vertexEntry) vertexEntry->Busy=FALSE;
                Nvx3dUnlockBuffers();status=ERROR_INVALID_PARAMETER;break;
            }
            depthEntry->Busy=TRUE;depthSaved=*depthEntry;
        }
        fenceValue=engine->Nvx3dLastFence+1;
        if(!fenceValue||fenceValue>0xffffffffULL) {
            entry->Busy=FALSE;
            if(vertexEntry) vertexEntry->Busy=FALSE;
            if(depthEntry) depthEntry->Busy=FALSE;
            Nvx3dUnlockBuffers();
            status=ERROR_DEVICE_NOT_READY;break;
        }
        engine->Nvx3dLastFence=fenceValue;
        Nvx3dUnlockBuffers();
        if(request.Operation==NVX_3D_SUBMIT_DRAW_TRIANGLE||
           request.Operation==NVX_3D_SUBMIT_DRAW_TRIANGLE_PRESERVE||
           request.Operation==NVX_3D_SUBMIT_DRAW_TRIANGLE_DEPTH||
           request.Operation==NVX_3D_SUBMIT_DRAW_TRIANGLES) {
            ULONG drawPixels=0,whitePixels=0;
            if(!NvxGspSubmitUserTriangleBuffer(engine,saved.GpuVa,
                    saved.DepthGpuVa,vertexSaved.GpuVa,request.Color,
                    request.Operation==NVX_3D_SUBMIT_DRAW_TRIANGLES?
                        request.Height:3U,
                    request.Operation!=NVX_3D_SUBMIT_DRAW_TRIANGLE,
                    request.Operation==NVX_3D_SUBMIT_DRAW_TRIANGLE_DEPTH,
                    (saved.Usage&NVX_3D_USAGE_DEPTH_METHOD_S8Z24)?
                        NVX_3D_ZT_FORMAT_S8Z24:
                    (saved.Usage&NVX_3D_USAGE_DEPTH_ZF32)?
                        NVX_3D_ZT_FORMAT_ZF32:
                    (saved.Usage&NVX_3D_USAGE_DEPTH_S8Z24)?
                        NVX_3D_ZT_FORMAT_X8Z24:NVX_3D_ZT_FORMAT_Z24S8,
                    fenceValue,
                    &drawPixels,&whitePixels)) {
                if(request.Operation==NVX_3D_SUBMIT_DRAW_TRIANGLE_DEPTH) {
                    LogStr("NVX [3D-DEPTH]: miniport draw submit failed.");
                    LogStr(" status=");LogHex(status);
                    LogStr(" dst=");LogHex(saved.GpuVa);
                    LogStr(" depth=");LogHex(saved.DepthGpuVa);
                    LogStr(" vertex=");LogHex(vertexSaved.GpuVa);
                    LogStr(" fence=");LogHex(fenceValue);LogStr("\n");
                }
                status=ERROR_DEVICE_NOT_READY;
            } else {
                request.Reserved[0]=drawPixels;
                request.Reserved[1]=whitePixels;
                request.FenceValue=fenceValue;
                VideoPortMoveMemory(RequestPacket->OutputBuffer,&request,sizeof(request));
                statusBlock->Information=sizeof(request);status=NO_ERROR;
                engine->GspGraphicsUserSubmitReady=TRUE;
                engine->GspGraphicsClientTriangleVerified=TRUE;
            }
        } else if(request.Operation==NVX_3D_SUBMIT_CLEAR_COLOR_TARGET_DEPTH) {
            if(!NvxGspSubmitUserColorTargetDepthClear(engine,
                    saved.DepthGpuVa,saved.Bytes-
                    NVX_3D_DEPTH_TARGET_OFFSET,fenceValue)) {
                status=ERROR_DEVICE_NOT_READY;
            } else {
                request.FenceValue=fenceValue;
                VideoPortMoveMemory(RequestPacket->OutputBuffer,&request,sizeof(request));
                statusBlock->Information=sizeof(request);status=NO_ERROR;
                engine->GspGraphicsUserSubmitReady=TRUE;
            }
        } else if(Nvx3dIsDynamicDepthDraw(request.Operation)) {
            ULONG vertexCount=request.Reserved[0];
            if(!NvxGspSubmitUserColorTargetDepthTriangles(engine,
                    saved.GpuVa,saved.Width,saved.Height,saved.TileMode,
                    depthSaved.DepthGpuVa,depthSaved.PitchBytes,
                    vertexSaved.GpuVa,vertexCount,request.Color,
                    Nvx3dDepthModeForOperation(request.Operation),fenceValue)) {
                LogStr("NVX [3D-DYNAMIC]: C797 submit failed color=");
                LogHex(saved.GpuVa);LogStr(" depth=");LogHex(depthSaved.GpuVa);
                LogStr(" depthPitch=");LogHex(depthSaved.PitchBytes);
                LogStr(" vertex=");LogHex(vertexSaved.GpuVa);
                LogStr(" count=");LogHex(vertexCount);LogStr(" mode=");
                LogHex(Nvx3dDepthModeForOperation(request.Operation));
                LogStr(" fence=");LogHex(fenceValue);LogStr("\n");
                status=ERROR_DEVICE_NOT_READY;
            } else {
                request.Reserved[0]=request.Reserved[1]=request.Reserved[2]=
                    request.Reserved[3]=request.Reserved[4]=request.Reserved[5]=0;
                request.FenceValue=fenceValue;
                VideoPortMoveMemory(RequestPacket->OutputBuffer,&request,sizeof(request));
                statusBlock->Information=sizeof(request);status=NO_ERROR;
                engine->GspGraphicsUserSubmitReady=TRUE;
                engine->GspGraphicsClientTriangleVerified=TRUE;
            }
        } else if(request.Operation==NVX_3D_SUBMIT_DRAW_COLOR_TARGET ||
                  request.Operation==NVX_3D_SUBMIT_VERIFY_COLOR_TARGET_DRAW) {
            ULONG vertexCount=request.Reserved[0];
            if(!NvxGspSubmitUserColorTargetTriangles(engine,saved.GpuVa,
                    saved.Width,saved.Height,saved.TileMode,vertexSaved.GpuVa,
                    vertexCount,request.Color,fenceValue,
                    request.Operation==NVX_3D_SUBMIT_VERIFY_COLOR_TARGET_DRAW)) {
                status=ERROR_DEVICE_NOT_READY;
            } else {
                request.Reserved[0]=request.Reserved[1]=
                    request.Reserved[2]=request.Reserved[3]=0;
                request.FenceValue=fenceValue;
                VideoPortMoveMemory(RequestPacket->OutputBuffer,&request,sizeof(request));
                statusBlock->Information=sizeof(request);status=NO_ERROR;
                engine->GspGraphicsUserSubmitReady=TRUE;
                engine->GspGraphicsClientTriangleVerified=TRUE;
            }
        } else if(request.Operation==NVX_3D_SUBMIT_CLEAR_DEPTH) {
            if(!NvxGspSubmitUserDepthClear(engine,saved.DepthGpuVa,
                    (saved.Usage&NVX_3D_USAGE_DEPTH_ZF32)!=0,fenceValue)) {
                LogStr("NVX [3D-DEPTH]: miniport clear submit failed.");
                LogStr(" depth=");LogHex(saved.DepthGpuVa);
                LogStr(" bytes=");LogHex(saved.Bytes);
                LogStr(" usage=");LogHex(saved.Usage);
                LogStr(" fence=");LogHex(fenceValue);LogStr("\n");
                status=ERROR_DEVICE_NOT_READY;
            } else {
                request.FenceValue=fenceValue;
                VideoPortMoveMemory(RequestPacket->OutputBuffer,&request,sizeof(request));
                statusBlock->Information=sizeof(request);status=NO_ERROR;
                engine->GspGraphicsUserSubmitReady=TRUE;
            }
        } else if(request.Operation==NVX_3D_SUBMIT_TEST_TRIANGLE) {
            ULONG greenPixels=0,whitePixels=0;
            if(!NvxGspSubmitUserTriangle(engine,saved.GpuVa,fenceValue,
                                          &greenPixels,&whitePixels)) {
                status=ERROR_DEVICE_NOT_READY;
            } else {
                request.Reserved[0]=greenPixels;
                request.Reserved[1]=whitePixels;
                request.FenceValue=fenceValue;
                VideoPortMoveMemory(RequestPacket->OutputBuffer,&request,sizeof(request));
                statusBlock->Information=sizeof(request);status=NO_ERROR;
                engine->GspGraphicsUserSubmitReady=TRUE;
            }
        } else if(request.Operation==NVX_3D_SUBMIT_CLEAR_COLOR_TARGET) {
            ULONG matchingSamples=0;
            if(!NvxGspSubmitUserColorTargetClear(engine,saved.GpuVa,
                    saved.Width,saved.Height,saved.TileMode,request.Color,
                    fenceValue,&matchingSamples)) {
                status=ERROR_DEVICE_NOT_READY;
            } else {
                request.Reserved[0]=matchingSamples;
                request.FenceValue=fenceValue;
                VideoPortMoveMemory(RequestPacket->OutputBuffer,&request,sizeof(request));
                statusBlock->Information=sizeof(request);status=NO_ERROR;
                engine->GspGraphicsUserSubmitReady=TRUE;
            }
        } else {
            ULONG matchingPixels=0;
            if(!NvxGspSubmitUserClear(engine,saved.GpuVa,request.Color,
                                      fenceValue,&matchingPixels)) {
                status=ERROR_DEVICE_NOT_READY;
            } else {
                request.Reserved[0]=matchingPixels;
                request.FenceValue=fenceValue;
                VideoPortMoveMemory(RequestPacket->OutputBuffer,&request,sizeof(request));
                statusBlock->Information=sizeof(request);status=NO_ERROR;
                engine->GspGraphicsUserSubmitReady=TRUE;
            }
        }
        if(Nvx3dLockBuffers()) {
            if(status==NO_ERROR&&request.ClientToken) {
                engine->Nvx3dLastSubmitToken=request.RequestToken;
                engine->Nvx3dLastSubmitClientToken=request.ClientToken;
                VideoPortMoveMemory(engine->Nvx3dLastSubmitReply,&request,
                                    sizeof(request));
            }
            entry=Nvx3dFindBuffer(engine,saved.Id);
            if(entry) entry->Busy=FALSE;
            if(vertexBufferId) {
                vertexEntry=Nvx3dFindBuffer(engine,vertexBufferId);
                if(vertexEntry) vertexEntry->Busy=FALSE;
            }
            if(depthBufferId) {
                depthEntry=Nvx3dFindBuffer(engine,depthBufferId);
                if(depthEntry) depthEntry->Busy=FALSE;
            }
            Nvx3dUnlockBuffers();
        }
        break;
    }

    case IOCTL_NVX_3D_WAIT_FENCE: {
        PNVX_DEVICE_EXTENSION engine=devExt->IsSecondaryDisplay?
            (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension:devExt;
        NVX_3D_WAIT_FENCE request;
        if(!RequestPacket->InputBuffer||
           RequestPacket->InputBufferLength!=sizeof(request)||
           !RequestPacket->OutputBuffer||
           RequestPacket->OutputBufferLength<sizeof(request)) {
            status=ERROR_INVALID_PARAMETER;break;
        }
        VideoPortMoveMemory(&request,RequestPacket->InputBuffer,sizeof(request));
        if(!Nvx3dValidateWait(&request)) {status=ERROR_INVALID_PARAMETER;break;}
        if(!NvxGspWaitUserFence(engine,request.FenceValue,request.TimeoutMs)) {
            status=ERROR_DEVICE_NOT_READY;break;
        }
        request.Status=0;
        VideoPortMoveMemory(RequestPacket->OutputBuffer,&request,sizeof(request));
        statusBlock->Information=sizeof(request);status=NO_ERROR;
        break;
    }


    case IOCTL_NVX_GET_LOG: {
#ifndef __VXDKERNEL__
        ULONG maxBytes = RequestPacket->OutputBufferLength;
        PUCHAR outBuf = (PUCHAR)RequestPacket->OutputBuffer;
        ULONG count = 0;
        ULONG64 end;
        if (!outBuf || !maxBytes) {
            status = ERROR_INVALID_PARAMETER;
            break;
        }
        end = __sync_fetch_and_add(&g_LogWriteSequence, 0);
        if (end - g_LogReadSequence > NVX_LOG_RING_BYTES) {
            g_LogReadSequence = end - NVX_LOG_RING_BYTES;
        }
        count = (ULONG)(end - g_LogReadSequence);
        if (count > maxBytes) count = maxBytes;
        for (ULONG i = 0; i < count; i++) {
            outBuf[i] = g_LogRing[(g_LogReadSequence + i) % NVX_LOG_RING_BYTES];
        }
        g_LogReadSequence += count;
        statusBlock->Information = count;
        status = NO_ERROR;
#else
        status = ERROR_INVALID_FUNCTION;
#endif
        break;
    }

    case IOCTL_NVX_ACCEL_UPLOAD: {
        PNVX_ACCEL_UPLOAD_PARAMS params=(PNVX_ACCEL_UPLOAD_PARAMS)RequestPacket->InputBuffer;
        PNVX_DEVICE_EXTENSION engine=devExt->IsSecondaryDisplay?
            (PNVX_DEVICE_EXTENSION)devExt->PrimaryDeviceExtension:devExt;
        static ULONG s_uploadKinds[4]={0,0,0,0};
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
        if(RequestPacket->InputBufferLength!=required||!engine||!engine->GspCopyVerified||
           devExt->CurrentModeIndex>=devExt->NumModes) {
            status=ERROR_NOT_SUPPORTED;
            break;
        }
        {
            ULONG64 base=engine->GspVramGpuVa+(devExt->IsSecondaryDisplay?0x2000000ULL:0ULL);
            ULONG64 surfaceBytes=(ULONG64)devExt->Modes[devExt->CurrentModeIndex].ScreenStride*
                devExt->Modes[devExt->CurrentModeIndex].VisScreenHeight;
            if(params->DstGpuVa<base || params->DstGpuVa-base>surfaceBytes ||
               params->Bytes>surfaceBytes-(params->DstGpuVa-base)) {
                status=ERROR_INVALID_PARAMETER;break;
            }
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
        if(NvxGspUpload(engine,params->DstGpuVa,params->Data,params->Bytes)) {
            status=NO_ERROR;
            statusBlock->Information=required;
            s_uploadKinds[params->Reserved]++;
            if ((s_uploadKinds[params->Reserved] <= 5) || (s_uploadKinds[params->Reserved] % 10000 == 0)) {
                LogStr("NVX [ACCEL]: secondary=");LogHex(devExt->IsSecondaryDisplay);
                LogStr(" GPU Upload kind=");LogHex(params->Reserved);
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
#ifdef _WIN64
typedef NTSTATUS (*PDRIVER_ADD_DEVICE)(PVOID DriverObject, PVOID PhysicalDeviceObject);
typedef NTSTATUS (*PDRIVER_DISPATCH)(PVOID DeviceObject, PVOID Irp);
#else
typedef NTSTATUS (__stdcall *PDRIVER_ADD_DEVICE)(PVOID DriverObject, PVOID PhysicalDeviceObject);
typedef NTSTATUS (__stdcall *PDRIVER_DISPATCH)(PVOID DeviceObject, PVOID Irp);
#endif

static PDRIVER_ADD_DEVICE RealAddDevice = NULL;
static PDRIVER_DISPATCH RealPnpDispatch = NULL;
static PDRIVER_DISPATCH RealPowerDispatch = NULL;

#ifdef _WIN64
static NTSTATUS HookedAddDevice(PVOID DriverObject, PVOID PhysicalDeviceObject) {
#else
static NTSTATUS __stdcall HookedAddDevice(PVOID DriverObject, PVOID PhysicalDeviceObject) {
#endif
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

#ifdef _WIN64
static NTSTATUS HookedPnpDispatch(PVOID DeviceObject, PVOID Irp) {
    unsigned char *irpBytes = (unsigned char *)Irp;
    unsigned char **stackLocationPtr = (unsigned char **)(irpBytes + 0xb8);
#else
static NTSTATUS __stdcall HookedPnpDispatch(PVOID DeviceObject, PVOID Irp) {
    unsigned char *irpBytes = (unsigned char *)Irp;
    unsigned char **stackLocationPtr = (unsigned char **)(irpBytes + 0x60);
#endif
    unsigned char *stack = *stackLocationPtr;
    unsigned char major = stack ? stack[0] : 0xFF;
    unsigned char minor = stack ? stack[1] : 0xFF;

    LogStr("NVX: HookedPnpDispatch! Major=");
    LogHex(major);
    LogStr(" Minor=");
    LogHex(minor);
    LogStr(" DevObj=");
    LogHex((unsigned long long)DeviceObject);
    LogStr("\n");

    NTSTATUS status = RealPnpDispatch(DeviceObject, Irp);

    LogStr("NVX: RealPnpDispatch Major=");
    LogHex(major);
    LogStr(" Minor=");
    LogHex(minor);
    LogStr(" returned: ");
    LogHex((unsigned long long)status);
    LogStr("\n");
    NvxLogFlush();
    return status;
}

#ifdef _WIN64
static NTSTATUS HookedPowerDispatch(PVOID DeviceObject, PVOID Irp) {
#else
static NTSTATUS __stdcall HookedPowerDispatch(PVOID DeviceObject, PVOID Irp) {
#endif
    LogStr("NVX: HookedPowerDispatch!\n");
    NTSTATUS status = RealPowerDispatch(DeviceObject, Irp);
    NvxLogFlush();
    return status;
}

#ifdef _WIN64
ULONG DriverEntry(PVOID Context1, PVOID Context2) {
#else
ULONG NTAPI DriverEntry(PVOID Context1, PVOID Context2) {
#endif
    VIDEO_HW_INITIALIZATION_DATA hwInitData;
    ULONG status;

    LogStr("\n========================================\n");
    LogStr(" NVX Video Miniport Driver 3.0.0.5 (open-gpu-kernel-modules unified v2)\n");
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

#ifdef __VXDKERNEL__
    NvxLogFlush();
    return status;
#else
    if (status == 0) {
        unsigned char *drvObj = (unsigned char *)Context1;
#ifdef _WIN64
        unsigned char *drvExt = *(unsigned char **)(drvObj + 0x30);
        ULONG addDeviceOffset = 0x08;
        ULONG pnpDispatchOffset = 0x148;
        ULONG powerDispatchOffset = 0x120;
#else
        unsigned char *drvExt = *(unsigned char **)(drvObj + 0x18);
        ULONG addDeviceOffset = 0x04;
        ULONG pnpDispatchOffset = 0xa4;
        ULONG powerDispatchOffset = 0x90;
#endif
        if (drvExt) {
            RealAddDevice = *(PDRIVER_ADD_DEVICE *)(drvExt + addDeviceOffset);
            LogStr("NVX: Original AddDevice: ");
            LogHex((unsigned long long)RealAddDevice);
            LogStr("\n");
            *(PDRIVER_ADD_DEVICE *)(drvExt + addDeviceOffset) = HookedAddDevice;

            /*
             * videoprt.sys VpIsMultiFunctionSupported (at offset 0x25ad0 on x64, 0xd368 on x86) checks
             * if the GPU is a multi-function PCI device (bit 7 of PCI HeaderType 0x0E).
             * For RTX 3070 (display + audio), it queries MultiFunctionSupported from the registry,
             * and if absent or 0, returns FALSE (0), causing IRP_MN_START_DEVICE to fail with
             * STATUS_UNSUCCESSFUL (0xC0000001 / Code 10) before calling HwVidFindAdapter!
             * Patch VpIsMultiFunctionSupported to unconditionally return TRUE (1).
             */
#ifdef _WIN64
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
#else
            if (RealAddDevice) {
                /*
                 * On 32-bit Windows XP videoprt.sys:
                 * RealAddDevice is at offset 0xea26 from videoprt base.
                 * VpIsMultiFunctionSupported is at offset 0xed8c from videoprt base.
                 * Exactly RealAddDevice + 0x366 bytes!
                 * Prologue: 8B FF 55 8B EC 81 EC 0C 01 00 00
                 */
                unsigned char *multiFunc = (unsigned char *)RealAddDevice + 0x366;
                BOOLEAN found = FALSE;

                if (multiFunc[0] == 0x8b && multiFunc[1] == 0xff && multiFunc[2] == 0x55 &&
                    multiFunc[3] == 0x8b && multiFunc[4] == 0xec && multiFunc[5] == 0x81 &&
                    multiFunc[6] == 0xec && multiFunc[7] == 0x0c && multiFunc[8] == 0x01) {
                    found = TRUE;
                } else {
                    /* Scan range [-0x1000, +0x1000] around RealAddDevice if layout shifts */
                    for (int off = -0x1000; off < 0x2000; off++) {
                        unsigned char *p = (unsigned char *)RealAddDevice + off;
                        if (p[0] == 0x8b && p[1] == 0xff && p[2] == 0x55 &&
                            p[3] == 0x8b && p[4] == 0xec && p[5] == 0x81 &&
                            p[6] == 0xec && p[7] == 0x0c && p[8] == 0x01) {
                            multiFunc = p;
                            found = TRUE;
                            break;
                        }
                    }
                }

                if (found) {
                    unsigned long cr0;
                    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
                    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0 & ~(1UL << 16)));

                    /* mov $1, %eax; ret $4 -> b8 01 00 00 00 c2 04 00 */
                    multiFunc[0] = 0xb8;
                    multiFunc[1] = 0x01;
                    multiFunc[2] = 0x00;
                    multiFunc[3] = 0x00;
                    multiFunc[4] = 0x00;
                    multiFunc[5] = 0xc2;
                    multiFunc[6] = 0x04;
                    multiFunc[7] = 0x00;

                    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0));
                    LogStr("NVX: Successfully Patched 32-bit VpIsMultiFunctionSupported at ");
                    LogHex((unsigned long long)multiFunc);
                    LogStr("\n");
                } else {
                    LogStr("NVX: Warning: 32-bit VpIsMultiFunctionSupported signature not matched at ");
                    LogHex((unsigned long long)multiFunc);
                    LogStr("\n");
                }
            }
#endif
        }

        RealPnpDispatch = *(PDRIVER_DISPATCH *)(drvObj + pnpDispatchOffset);
        LogStr("NVX: Original PnpDispatch: ");
        LogHex((unsigned long long)RealPnpDispatch);
        LogStr("\n");
        *(PDRIVER_DISPATCH *)(drvObj + pnpDispatchOffset) = HookedPnpDispatch;

        RealPowerDispatch = *(PDRIVER_DISPATCH *)(drvObj + powerDispatchOffset);
        *(PDRIVER_DISPATCH *)(drvObj + powerDispatchOffset) = HookedPowerDispatch;
    }
#endif

    NvxLogFlush();
    return status;
}
