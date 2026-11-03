#include "nvx_display.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    assert(NvxDisplayPutBytes(61)==244 && NvxDisplayGetWords(244)==61);
    assert(sizeof(NVX_DFP_ASSIGN_SOR_PARAMS)==80);
    assert(offsetof(NVX_DFP_ASSIGN_SOR_PARAMS,sorExcludeMask)==8);
    assert(offsetof(NVX_DFP_ASSIGN_SOR_PARAMS,slaveDisplayId)==12);
    assert(offsetof(NVX_DFP_ASSIGN_SOR_PARAMS,sorAssignList)==24);
    assert(offsetof(NVX_DFP_ASSIGN_SOR_PARAMS,sorAssignListWithTag)==40);
    assert(offsetof(NVX_DFP_ASSIGN_SOR_PARAMS,flags)==76);
    assert(NVX_CTRL_CMD_DFP_ASSIGN_SOR==0x00731152U);
    assert(sizeof(NVX_DP_AUX_CTRL_PARAMS)==48);
    assert(offsetof(NVX_DP_AUX_CTRL_PARAMS,data)==20);
    assert(offsetof(NVX_DP_AUX_CTRL_PARAMS,size)==36);
    assert(NVX_CTRL_CMD_DP_AUXCH_CTRL==0x00731341U);
    assert(NVX_CTRL_CMD_DP_CTRL==0x00731343U);
    assert(NVX_CTRL_CMD_DP_GET_LINK_CONFIG==0x00731360U);
    assert(NVX_CTRL_CMD_DP_GET_CAPS==0x00731369U);
    assert(sizeof(NVX_DP_DSC_CAPS)==28);
    assert(sizeof(NVX_DP_GET_CAPS_PARAMS)==56);
    assert(offsetof(NVX_DP_GET_CAPS_PARAMS,maxLinkRate)==8);
    assert(offsetof(NVX_DP_GET_CAPS_PARAMS,dpVersionsSupported)==12);
    assert(offsetof(NVX_DP_GET_CAPS_PARAMS,dsc)==28);
    assert(sizeof(NVX_DP_CTRL_PARAMS)==28);
    assert(NVX_DP_CTRL_SET_LANE_COUNT==1 && NVX_DP_CTRL_SET_LINK_BW==2);
    assert(NVX_DP_CTRL_SET_ENHANCED_FRAMING==0x80);
    assert(NVX_DP_CTRL_DATA_LINK_BW_HBR2==0x1400);
    assert(NVX_DP_CTRL_DATA_ENHANCED_FRAMING==0x40000);
    assert(NVX_CTRL_CMD_DP_MAIN_LINK_CTRL==0x00731356U);
    assert(sizeof(NVX_DP_MAIN_LINK_CTRL_PARAMS)==12);
    assert(NVX_C67D_HEAD_SET_CONTROL(1)==0x2408U);
    assert(NVX_C67D_HEAD_SET_DISPLAY_ID(1)==0x2420U);
    assert(NVX_C67D_HEAD_SET_HEAD_USAGE_BOUNDS(1)==0x2430U);
    assert(NVX_C67D_HEAD_SET_VIEWPORT_POINT_IN(1)==0x2448U);
    assert(NVX_C67D_HEAD_SET_VIEWPORT_SIZE_IN(1)==0x244CU);
    assert(NVX_C67D_HEAD_SET_VIEWPORT_SIZE_OUT(1)==0x2458U);
    assert(NVX_C67D_HEAD_SET_VIEWPORT_POINT_OUT_ADJUST(1)==0x245CU);
    assert(NVX_C67D_UPDATE_SPECIAL_HANDLING_INTERRUPT_RM == 0x00100000U);
    assert(NVX_C67D_UPDATE_INHIBIT_INTERRUPTS == 0x01000000U);
    /* 1920x1080p60, 148.5MHz, totals 2200x1125, +H/+V. */
    uint8_t d[18]={0x02,0x3a,0x80,0x18,0x71,0x38,0x2d,0x40,
                   0x58,0x2c,0x45,0,0,0,0,0,0,0x1e};
    NVX_DISPLAY_TIMING t,unchanged;
    assert(NvxDecodeDetailedTiming(d,sizeof(d),&t));
    assert(t.pixelClockHz==148500000 && t.hActive==1920 && t.vActive==1080);
    assert(t.hTotal==2200 && t.vTotal==1125 && t.refreshMilliHz==60000);
    assert(t.hSyncStart==2008 && t.hSyncEnd==2052);
    assert(t.vSyncStart==1084 && t.vSyncEnd==1089);
    {
        NVX_EVO_RASTER r;
        assert(NvxEncodeEvoRaster(&t,&r));
        assert(r.size==((1125U<<16)|2200U));
        assert(r.syncEnd==((4U<<16)|43U));
        assert(r.blankEnd==((40U<<16)|191U));
        assert(r.blankStart==((1120U<<16)|2111U));
        NVX_DISPLAY_TIMING bad=t;bad.hSyncStart=bad.hActive;
        assert(!NvxEncodeEvoRaster(&bad,&r));
    }
    {
        NVX_DP_CONFIG_STREAM_PARAMS p={0};
        assert(NvxDpBuildSst(&t,0,1,&p));
        assert(p.head==1 && p.sorIndex==0 && p.dpLink==1 && p.enableOverride==1);
        assert(p.sst.tuSize==64 && p.sst.waterMark==20);
        assert(p.hBlankSym==960 && p.vBlankSym==6823);
        assert(p.sst.actualPclkHz==148500000 && p.sst.linkClkFreqHz==540000000);
        assert(!NvxDpBuildSst(&t,4,1,&p));
        NVX_DISPLAY_TIMING bad=t;bad.pixelClockHz=1000000000;
        assert(!NvxDpBuildSst(&bad,0,1,&p));
    }
    unchanged=t; d[17]|=0x80;
    assert(!NvxDecodeDetailedTiming(d,sizeof(d),&t));
    assert(!memcmp(&t,&unchanged,sizeof(t)));
    d[17]=0x1e;d[9]=255; /* sync extends past horizontal blank */
    assert(!NvxDecodeDetailedTiming(d,sizeof(d),&t));
    assert(!NvxDecodeDetailedTiming(d,17,&t));
    memset(d,0,sizeof(d));assert(!NvxDecodeDetailedTiming(d,sizeof(d),&t));
    {
        uint32_t slot=0,entry[2]={0},header=0;
        assert(NvxDisplayPushHeader(0x208,1,&header) && header==0x40208);
        assert(NvxDisplayPushHeader(0x218,2,&header) && header==0x80218);
        assert(NvxDisplayPushHeader(0x200,1,&header) && header==0x40200);
        assert(NvxDisplayPushHeader(0x2030,1,&header) && header==0x42030);
        assert(!NvxDisplayPushHeader(0x209,1,&header));
        assert(!NvxDisplayPushHeader(0x4000,1,&header));
        assert(!NvxDisplayPushHeader(0x200,0,&header));
        assert(NvxDisplayRamhtEntry(0xd1500001,0,0x2000,0xc1d00001,&slot,entry));
        assert(slot==0x117);
        assert(entry[0]==0xd1500001 && entry[1]==0x400001);
        assert(NvxDisplayRamhtEntry(0xd1500001,3,0x2020,0xc1d00001,&slot,entry));
        assert(entry[1]==0x06404001);
        assert(!NvxDisplayRamhtEntry(1,16,0x2000,1,&slot,entry));
        assert(!NvxDisplayRamhtEntry(0,0,0x2000,1,&slot,entry));
        assert(!NvxDisplayRamhtEntry(1,0,0x1000,1,&slot,entry));
        assert(!NvxDisplayRamhtEntry(1,0,0x2001,1,&slot,entry));
        assert(!NvxDisplayRamhtEntry(1,0,0x10000,1,&slot,entry));
    }
    puts("Display timing tests passed: 1080p60 totals/sync/clock, interlace, malformed sync, truncation and empty descriptor.");
}
