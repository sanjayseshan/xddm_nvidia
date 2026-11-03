#include "nvx_display.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    /* 1920x1080p60, 148.5MHz, totals 2200x1125, +H/+V. */
    uint8_t d[18]={0x02,0x3a,0x80,0x18,0x71,0x38,0x2d,0x40,
                   0x58,0x2c,0x45,0,0,0,0,0,0,0x1e};
    NVX_DISPLAY_TIMING t,unchanged;
    assert(NvxDecodeDetailedTiming(d,sizeof(d),&t));
    assert(t.pixelClockHz==148500000 && t.hActive==1920 && t.vActive==1080);
    assert(t.hTotal==2200 && t.vTotal==1125 && t.refreshMilliHz==60000);
    assert(t.hSyncStart==2008 && t.hSyncEnd==2052);
    assert(t.vSyncStart==1084 && t.vSyncEnd==1089);
    unchanged=t; d[17]|=0x80;
    assert(!NvxDecodeDetailedTiming(d,sizeof(d),&t));
    assert(!memcmp(&t,&unchanged,sizeof(t)));
    d[17]=0x1e;d[9]=255; /* sync extends past horizontal blank */
    assert(!NvxDecodeDetailedTiming(d,sizeof(d),&t));
    assert(!NvxDecodeDetailedTiming(d,17,&t));
    memset(d,0,sizeof(d));assert(!NvxDecodeDetailedTiming(d,sizeof(d),&t));
    puts("Display timing tests passed: 1080p60 totals/sync/clock, interlace, malformed sync, truncation and empty descriptor.");
}
