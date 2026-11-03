#ifndef NVX_DISPLAY_H
#define NVX_DISPLAY_H
#include <stdint.h>
#include <stddef.h>

#define NVX_C67D_UPDATE_SPECIAL_HANDLING_INTERRUPT_RM (1U << 20)

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
