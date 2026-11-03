#ifndef NVX_DISPLAY_H
#define NVX_DISPLAY_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t pixelClockHz;
    uint32_t hActive,hTotal,hSyncStart,hSyncEnd;
    uint32_t vActive,vTotal,vSyncStart,vSyncEnd;
    uint32_t refreshMilliHz;
    uint8_t flags;
} NVX_DISPLAY_TIMING;

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
