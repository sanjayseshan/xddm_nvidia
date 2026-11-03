#ifndef NVX_DISPLAY_H
#define NVX_DISPLAY_H
#include <stdint.h>
#include <stddef.h>

#define NVX_C67D_UPDATE_SPECIAL_HANDLING_INTERRUPT_RM (1U << 20)
#define NVX_C67D_UPDATE_INHIBIT_INTERRUPTS (1U << 24)

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
#define NVX_DP_AUX_REPLY_ACK 0U

typedef struct {
    uint32_t pixelClockHz;
    uint32_t hActive,hTotal,hSyncStart,hSyncEnd;
    uint32_t vActive,vTotal,vSyncStart,vSyncEnd;
    uint32_t refreshMilliHz;
    uint8_t flags;
} NVX_DISPLAY_TIMING;

/* Volta+ display RAMHT: 8KB / 8-byte entries, channel tag and 10-bit
 * XOR hash. Instance offsets are relative to the 64KB display block.
 * Encoding reference: Nouveau r535_dmac_bind and GV100 display DMA. */
static inline int NvxDisplayPushHeader(uint32_t method,uint32_t count,uint32_t *header)
{
    if(!header || !count || count>1023 || (method&3) || method>0x3ffc) return 0;
    *header=(count<<18)|(method>>2);
    return 1;
}

static inline int NvxDisplayRamhtEntry(uint32_t handle,uint32_t channel,
    uint32_t offset,uint32_t client,uint32_t *slot,uint32_t entry[2])
{
    if(!handle || channel>15 || offset<0x2000 || offset>0xffe0 ||
       (offset&15) || !slot || !entry) return 0;
    *slot=(handle^(handle>>10)^(handle>>20)^(handle>>30)^(channel<<6))&1023;
    entry[0]=handle;
    entry[1]=(channel<<25)|(offset<<9)|(client&0x3fff);
    return 1;
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
