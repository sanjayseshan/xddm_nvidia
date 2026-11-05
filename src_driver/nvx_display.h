#ifndef NVX_DISPLAY_H
#define NVX_DISPLAY_H
#include <stdint.h>
#include <stddef.h>

#define NVX_C67D_UPDATE_SPECIAL_HANDLING_INTERRUPT_RM (1U << 20)
#define NVX_C67D_UPDATE_INHIBIT_INTERRUPTS (1U << 24)

/* GA104 / C67D Core Methods */
#define NVX_C67D_HEAD_SET_CONTROL_OUTPUT_RESOURCE(head) (0x2004U + (head) * 0x400U)
#define NVX_C67D_HEAD_SET_CONTROL(head)                 (0x2008U + (head) * 0x400U)
#define NVX_C67D_HEAD_SET_PIXEL_CLOCK_FREQUENCY(head)   (0x200CU + (head) * 0x400U)
#define NVX_C67D_HEAD_SET_PIXEL_CLOCK_CONFIGURATION(head) (0x201CU + (head) * 0x400U)
#define NVX_C67D_HEAD_SET_DISPLAY_ID(head)              (0x2020U + (head) * 0x400U)
#define NVX_C67D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX(head) (0x2028U + (head) * 0x400U)
#define NVX_C67D_HEAD_SET_HEAD_USAGE_BOUNDS(head)       (0x2030U + (head) * 0x400U)
#define NVX_C67D_HEAD_SET_VIEWPORT_POINT_IN(head)       (0x2048U + (head) * 0x400U)
#define NVX_C67D_HEAD_SET_VIEWPORT_SIZE_IN(head)        (0x204CU + (head) * 0x400U)
#define NVX_C67D_HEAD_SET_VIEWPORT_SIZE_OUT(head)       (0x2058U + (head) * 0x400U)
#define NVX_C67D_HEAD_SET_VIEWPORT_POINT_OUT_ADJUST(head) (0x205CU + (head) * 0x400U)
#define NVX_C67D_HEAD_SET_RASTER_SIZE(head)             (0x2064U + (head) * 0x400U)
#define NVX_C67D_HEAD_SET_RASTER_SYNC_END(head)         (0x2068U + (head) * 0x400U)
#define NVX_C67D_HEAD_SET_RASTER_BLANK_END(head)        (0x206CU + (head) * 0x400U)
#define NVX_C67D_HEAD_SET_RASTER_BLANK_START(head)      (0x2070U + (head) * 0x400U)
#define NVX_C67D_HEAD_SET_OVERSCAN_COLOR(head)          (0x2078U + (head) * 0x400U)
#define NVX_C67D_HEAD_SET_MIN_FRAME_IDLE(head)          (0x2218U + (head) * 0x400U)

#ifndef IOCTL_VIDEO_GET_CHILD_STATE
#define IOCTL_VIDEO_GET_CHILD_STATE                     0x00230480U
#endif
#ifndef IOCTL_VIDEO_VALIDATE_CHILD_STATE_CONFIGURATION
#define IOCTL_VIDEO_VALIDATE_CHILD_STATE_CONFIGURATION  0x00230484U
#endif
#ifndef IOCTL_VIDEO_SET_CHILD_STATE_CONFIGURATION
#define IOCTL_VIDEO_SET_CHILD_STATE_CONFIGURATION       0x00230488U
#endif
#ifndef VIDEO_CHILD_ACTIVE
#define VIDEO_CHILD_ACTIVE                              0x00000001U
#endif
#ifndef VIDEO_CHILD_DETACHED
#define VIDEO_CHILD_DETACHED                            0x00000002U
#endif

typedef struct _VIDEO_CHILD_STATE {
    uint32_t Id;
    uint32_t State;
} VIDEO_CHILD_STATE, *PVIDEO_CHILD_STATE;

typedef struct _VIDEO_CHILD_STATE_CONFIGURATION {
    uint32_t Count;
    VIDEO_CHILD_STATE ChildStateArray[1];
} VIDEO_CHILD_STATE_CONFIGURATION, *PVIDEO_CHILD_STATE_CONFIGURATION;

#define NVX_C67D_SOR_SET_CONTROL(sor)                   (0x300U + (sor) * 0x20U)
#define NVX_C67D_WINDOW_SET_CONTROL(win)                (0x1000U + (win) * 0x80U)

#define NVX_C67D_UPDATE                                 0x200U
#define NVX_C67D_SET_INTERLOCK_FLAGS                    0x218U
#define NVX_C67D_SET_WINDOW_INTERLOCK_FLAGS             0x21CU

/* GA104 / C67E Window Methods */
#define NVX_C67E_SET_SIZE                               0x224U
#define NVX_C67E_SET_STORAGE                            0x228U
#define NVX_C67E_SET_PARAMS                             0x22CU
#define NVX_C67E_SET_PLANAR_STORAGE(plane)              (0x230U + (plane) * 4U)
#define NVX_C67E_SET_CONTEXT_DMA_ISO(plane)             (0x240U + (plane) * 4U)
#define NVX_C67E_SET_OFFSET(plane)                      (0x260U + (plane) * 4U)
#define NVX_C67E_SET_SIZE_IN                            0x298U
#define NVX_C67E_SET_SIZE_OUT                           0x2A4U
#define NVX_C67E_UPDATE                                 0x200U
#define NVX_C67E_SET_INTERLOCK_FLAGS                    0x370U

/* NVIDIA RM ctrl0073dfp.h, NV0073_CTRL_CMD_DFP_ASSIGN_SOR (0x731152).
 * NvBool is a 32-bit RM boolean; natural 4-byte alignment matches the wire
 * structure used by the GSP RM control RPC. */
typedef struct {
    uint32_t subDeviceInstance;
    uint32_t displayId;
    uint8_t sorExcludeMask;
    uint8_t reserved0[3];
    uint32_t slaveDisplayId;
    uint32_t forceSublinkConfig;
    uint32_t is2Head1Or;
    uint32_t sorAssignList[4];
    struct { uint32_t displayMask; uint32_t sorType; } sorAssignListWithTag[4];
    uint8_t reservedSorMask;
    uint8_t reserved1[3];
    uint32_t flags;
} NVX_DFP_ASSIGN_SOR_PARAMS;

_Static_assert(sizeof(NVX_DFP_ASSIGN_SOR_PARAMS)==80,"DFP_ASSIGN_SOR RM ABI");
_Static_assert(offsetof(NVX_DFP_ASSIGN_SOR_PARAMS,slaveDisplayId)==12,"DFP_ASSIGN_SOR slave offset");
_Static_assert(offsetof(NVX_DFP_ASSIGN_SOR_PARAMS,sorAssignList)==24,"DFP_ASSIGN_SOR map offset");
_Static_assert(offsetof(NVX_DFP_ASSIGN_SOR_PARAMS,sorAssignListWithTag)==40,"DFP_ASSIGN_SOR tagged map offset");
_Static_assert(offsetof(NVX_DFP_ASSIGN_SOR_PARAMS,flags)==76,"DFP_ASSIGN_SOR flags offset");

#define NVX_CTRL_CMD_DFP_ASSIGN_SOR 0x00731152U
#define NVX_DFP_SOR_TYPE_NONE 0U
#define NVX_DFP_SOR_TYPE_SINGLE 1U
#define NVX_DFP_SOR_TYPE_2H1OR_PRIMARY 2U
#define NVX_DFP_SOR_TYPE_2H1OR_SECONDARY 3U

/* ctrl0073dp.h read-only DisplayPort controls used before link training. */
#define NVX_CTRL_CMD_DP_AUXCH_CTRL 0x00731341U
#define NVX_CTRL_CMD_DP_CTRL 0x00731343U
#define NVX_CTRL_CMD_DP_GET_LINK_CONFIG 0x00731360U
#define NVX_CTRL_CMD_DP_GET_CAPS 0x00731369U
typedef struct {
    uint8_t supported;
    uint32_t encoderColorFormatMask;
    uint32_t lineBufferSizeKB;
    uint32_t rateBufferSizeKB;
    uint32_t bitsPerPixelPrecision;
    uint32_t maxNumHztSlices;
    uint32_t lineBufferBitDepth;
} NVX_DP_DSC_CAPS;
typedef struct {
    uint32_t subDeviceInstance;
    uint32_t sorIndex;
    uint32_t maxLinkRate;
    uint32_t dpVersionsSupported;
    uint32_t uhbrSupported;
    uint8_t multistreamSupported;
    uint8_t streamCloningEnabled;
    uint8_t increasedWatermarkLimits;
    uint8_t pc2Disabled;
    uint8_t singleHeadMstSupported;
    uint8_t fecSupported;
    uint8_t trainPhyRepeater;
    uint8_t overrideLinkBandwidth;
    NVX_DP_DSC_CAPS dsc;
} NVX_DP_GET_CAPS_PARAMS;
_Static_assert(sizeof(NVX_DP_DSC_CAPS)==28,"DP DSC caps RM ABI");
_Static_assert(sizeof(NVX_DP_GET_CAPS_PARAMS)==56,"DP GET_CAPS RM ABI");
_Static_assert(offsetof(NVX_DP_GET_CAPS_PARAMS,maxLinkRate)==8,"DP GET_CAPS rate offset");
_Static_assert(offsetof(NVX_DP_GET_CAPS_PARAMS,dpVersionsSupported)==12,"DP GET_CAPS version offset");
_Static_assert(offsetof(NVX_DP_GET_CAPS_PARAMS,dsc)==28,"DP GET_CAPS DSC offset");
typedef struct {
    uint32_t subDeviceInstance;
    uint32_t displayId;
    uint32_t cmd;
    uint32_t data;
    uint32_t err;
    uint32_t retryTimeMs;
    uint32_t eightLaneDpcdBaseAddr;
} NVX_DP_CTRL_PARAMS;
_Static_assert(sizeof(NVX_DP_CTRL_PARAMS)==28,"DP CTRL RM ABI");
#define NVX_DP_CTRL_SET_LANE_COUNT (1U<<0)
#define NVX_DP_CTRL_SET_LINK_BW (1U<<1)
#define NVX_DP_CTRL_SET_ENHANCED_FRAMING (1U<<7)
#define NVX_DP_CTRL_DATA_LANE_COUNT_4 4U
#define NVX_DP_CTRL_DATA_LINK_BW_HBR2 (0x14U<<8)
#define NVX_DP_CTRL_DATA_ENHANCED_FRAMING (1U<<18)
typedef struct {
    uint32_t subDeviceInstance;
    uint32_t displayId;
    uint32_t addrOnly;
    uint32_t cmd;
    uint32_t addr;
    uint8_t data[16];
    uint32_t size;
    uint32_t replyType;
    uint32_t retryTimeMs;
} NVX_DP_AUX_CTRL_PARAMS;
_Static_assert(sizeof(NVX_DP_AUX_CTRL_PARAMS)==48,"DP AUX RM ABI");
_Static_assert(offsetof(NVX_DP_AUX_CTRL_PARAMS,data)==20,"DP AUX data offset");
_Static_assert(offsetof(NVX_DP_AUX_CTRL_PARAMS,size)==36,"DP AUX size offset");
#define NVX_DP_AUX_TYPE_AUX 1U
#define NVX_DP_AUX_READ 1U
#define NVX_DP_AUX_WRITE 0U
#define NVX_DP_AUX_REPLY_ACK 0U

#define NVX_CTRL_CMD_DP_MAIN_LINK_CTRL 0x00731356U
#define NVX_DP_MAIN_LINK_CTRL_POWER_STATE_POWERUP 1U
#define NVX_DP_MAIN_LINK_CTRL_POWER_STATE_POWERDOWN 0U

typedef struct {
    uint32_t subDeviceInstance;
    uint32_t displayId;
    uint32_t ctrl;
} NVX_DP_MAIN_LINK_CTRL_PARAMS;
_Static_assert(sizeof(NVX_DP_MAIN_LINK_CTRL_PARAMS)==12,"DP MAIN_LINK_CTRL RM ABI");

typedef struct {
    uint32_t pixelClockHz;
    uint32_t hActive,hTotal,hSyncStart,hSyncEnd;
    uint32_t vActive,vTotal,vSyncStart,vSyncEnd;
    uint32_t refreshMilliHz;
    uint8_t flags;
} NVX_DISPLAY_TIMING;

/* EVO raster origin is one pixel/line inside sync, not the active image. */
typedef struct { uint32_t size,syncEnd,blankEnd,blankStart; } NVX_EVO_RASTER;
static inline int NvxEncodeEvoRaster(const NVX_DISPLAY_TIMING *t,NVX_EVO_RASTER *r)
{
    uint32_t hx,vy;
    if(!t || !r || !t->hActive || !t->vActive ||
       t->hActive>=t->hSyncStart || t->hSyncStart>=t->hSyncEnd ||
       t->hSyncEnd>t->hTotal || t->vActive>=t->vSyncStart ||
       t->vSyncStart>=t->vSyncEnd || t->vSyncEnd>t->vTotal ||
       t->hTotal>32767 || t->vTotal>32767) return 0;
    hx=t->hTotal-t->hSyncStart-1;
    vy=t->vTotal-t->vSyncStart-1;
    r->size=(t->vTotal<<16)|t->hTotal;
    r->syncEnd=((t->vSyncEnd-t->vSyncStart-1)<<16)|
                (t->hSyncEnd-t->hSyncStart-1);
    r->blankEnd=(vy<<16)|hx;
    r->blankStart=((vy+t->vActive)<<16)|(hx+t->hActive);
    return 1;
}

/* NVIDIA ctrl0073dp.h: byte-sized NvBool fields, natural 4-byte alignment. */
#define NVX_CTRL_CMD_DP_CONFIG_STREAM 0x00731362U
typedef struct {
    uint32_t subDeviceInstance,head,sorIndex,dpLink;
    uint8_t enableOverride,mst;
    uint32_t singleHeadMultistreamMode,hBlankSym,vBlankSym,colorFormat;
    uint8_t enableTwoHeadOneOr;
    uint32_t mstReserved[7];
    struct {
        uint8_t enhancedFraming;
        uint32_t tuSize,waterMark,actualPclkHz,linkClkFreqHz;
        uint8_t audioOverRightPanel;
        struct {
            uint32_t activeCnt,activeFrac,activePolarity;
            uint8_t mvidWarEnabled;
            uint32_t actualPclkHz,linkClkFreqHz;
        } legacy;
    } sst;
} NVX_DP_CONFIG_STREAM_PARAMS;
_Static_assert(sizeof(NVX_DP_CONFIG_STREAM_PARAMS)==116,"DP stream RM ABI");
_Static_assert(offsetof(NVX_DP_CONFIG_STREAM_PARAMS,hBlankSym)==24,"DP stream blank ABI");
_Static_assert(offsetof(NVX_DP_CONFIG_STREAM_PARAMS,sst)==68,"DP stream SST ABI");
/* RGB888, four-lane HBR2 transport. Auto-TU equations from NVIDIA DP library.
   Reject unsupported bandwidth/blanking rather than overriding with guesses. */
static inline int NvxDpBuildSst(const NVX_DISPLAY_TIMING *t,uint32_t sor,
                                uint32_t link,NVX_DP_CONFIG_STREAM_PARAMS *p)
{
    const uint64_t clock=540000000,precision=100000;
    uint64_t ratio,wf;
    uint32_t minBlank,hb,vb,water;
    NVX_EVO_RASTER raster;
    if(!p || sor>=4 || link>1 || !NvxEncodeEvoRaster(t,&raster) ||
       !t->pixelClockHz || t->hActive<=60 ||
       (uint64_t)t->pixelClockHz*24>=clock*32) return 0;
    ratio=(uint64_t)t->pixelClockHz*24*precision/(clock*32);
    wf=ratio*64*(precision-ratio)/precision;
    water=2+(2*(24*precision/32)+wf)/precision;
    if(water<20) water=20;
    if(water>39) return 0;
    minBlank=(uint32_t)(9ULL*t->pixelClockHz/clock)+12;
    if(t->hTotal-t->hActive<=minBlank) return 0;
    hb=(uint32_t)((t->hTotal-t->hActive-minBlank)*clock/t->pixelClockHz);
    vb=(uint32_t)((t->hActive-40)*clock/t->pixelClockHz);
    if(hb<=7 || vb<=13) return 0;
    p->head=1;p->sorIndex=sor;p->dpLink=link;p->enableOverride=1;
    p->hBlankSym=hb-7;p->vBlankSym=vb-13;p->colorFormat=0;
    p->sst.enhancedFraming=1;p->sst.tuSize=64;p->sst.waterMark=water;
    p->sst.actualPclkHz=t->pixelClockHz;p->sst.linkClkFreqHz=(uint32_t)clock;
    return 1;
}

/* Volta+ display RAMHT: 8KB / 8-byte entries, channel tag and 10-bit
 * XOR hash. Instance offsets are relative to the 64KB display block.
 * Encoding reference: Nouveau r535_dmac_bind and GV100 display DMA. */
static inline int NvxDisplayPushHeader(uint32_t method,uint32_t count,uint32_t *header)
{
    if(!header || !count || count>1023 || (method&3) || method>0x3ffc) return 0;
    *header=(count<<18)|method;
    return 1;
}

/* MMIO pointer packing follows Nouveau NV507C PUT/GET PTR bits 11:2. */
static inline uint32_t NvxDisplayPutBytes(uint32_t words) { return words<<2; }
static inline uint32_t NvxDisplayGetWords(uint32_t raw) { return (raw>>2)&1023; }

static inline int NvxDisplayRamhtEntry(uint32_t handle,uint32_t channel,
    uint32_t offset,uint32_t client,uint32_t *slot,uint32_t entry[2])
{
    if(!handle || channel>127 || offset<0x2000 || offset>0xffe0 ||
       (offset&31) || !slot || !entry) return 0;
    /* NVIDIA instmemHashFunc_v03_00: the lookup key includes the client
       stored in the context word, not just the object handle and channel. */
    *slot=((handle&0x3ff)^((handle>>10)&0x3ff)^((handle>>20)&0x3ff)^
           (((client&0xff)<<2)|(handle>>30))^
           (((channel&15)<<6)|((client>>8)&0x3f))^((channel>>4)&7));
    entry[0]=handle;
    entry[1]=(channel<<25)|(offset<<9)|(client&0x3fff);
    return 1;
}

/* Nouveau/v169 bucket layout, retained for the fixed three display objects.
   Context words are identical; both buckets point at the same descriptor. */
static inline uint32_t NvxDisplayRamhtV169Slot(uint32_t handle,uint32_t channel)
{
    return (handle^(handle>>10)^(handle>>20)^(handle>>30)^(channel<<6))&1023;
}

/* Decode a progressive, separate-sync EDID detailed timing descriptor.
 * An invalid descriptor must not become a hardware modeset request. */
static inline int NvxDecodeDetailedTiming(const uint8_t *d,size_t length,
                                          NVX_DISPLAY_TIMING *out)
{
    NVX_DISPLAY_TIMING t={0};
    uint32_t hb,vb,hs,hw,vs,vw;
    if(!d || !out || length<18 || !(d[0]|d[1])) return 0;
    if((d[17]&0x80) || (d[17]&0x18)!=0x18) return 0;
    t.pixelClockHz=((uint32_t)d[0]|((uint32_t)d[1]<<8))*10000U;
    t.hActive=d[2]|((uint32_t)(d[4]&0xf0)<<4);
    hb=d[3]|((uint32_t)(d[4]&15)<<8);
    t.vActive=d[5]|((uint32_t)(d[7]&0xf0)<<4);
    vb=d[6]|((uint32_t)(d[7]&15)<<8);
    hs=d[8]|((uint32_t)(d[11]&0xc0)<<2);
    hw=d[9]|((uint32_t)(d[11]&0x30)<<4);
    vs=(d[10]>>4)|((uint32_t)(d[11]&12)<<2);
    vw=(d[10]&15)|((uint32_t)(d[11]&3)<<4);
    if(t.hActive<640 || t.vActive<480 || !hs || !hw || !vs || !vw ||
       hs+hw>hb || vs+vw>vb) return 0;
    t.hTotal=t.hActive+hb;t.vTotal=t.vActive+vb;
    t.hSyncStart=t.hActive+hs;t.hSyncEnd=t.hSyncStart+hw;
    t.vSyncStart=t.vActive+vs;t.vSyncEnd=t.vSyncStart+vw;
    t.refreshMilliHz=(uint32_t)(((uint64_t)t.pixelClockHz*1000)/
                              ((uint64_t)t.hTotal*t.vTotal));
    if(t.refreshMilliHz<20000 || t.refreshMilliHz>500000) return 0;
    t.flags=d[17];*out=t;return 1;
}
#endif
