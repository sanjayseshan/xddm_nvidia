#include "../opengl_icd.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    NVX_OPENGL_DRIVER_INFO info;
    const size_t legacyBytes = offsetof(NVX_OPENGL_DRIVER_INFO, Name) +
                               256U * sizeof(uint16_t);
    memset(&info,0xa5,sizeof(info));
    assert(!NvxBuildOpenGlDriverInfo(1,&info,sizeof(info)));
    assert(!NvxBuildOpenGlDriverInfo(NVX_OPENGL_GETINFO_DRVNAME,NULL,sizeof(info)));
    assert(!NvxBuildOpenGlDriverInfo(NVX_OPENGL_GETINFO_DRVNAME,&info,legacyBytes-1));
    assert((size_t)NvxBuildOpenGlDriverInfo(NVX_OPENGL_GETINFO_DRVNAME,&info,legacyBytes)==legacyBytes);
    assert(info.Version==2 && info.DriverVersion==0x00010000U);
    assert(info.Name[0]=='n' && info.Name[6]=='p' && info.Name[7]==0);
    assert(NvxBuildOpenGlDriverInfo(NVX_OPENGL_GETINFO_DRVNAME,&info,sizeof(info))==sizeof(info));
    assert(info.Version==2 && info.DriverVersion==0x00010000U);
    assert(info.Name[0]=='n' && info.Name[1]=='v' && info.Name[2]=='x');
    assert(info.Name[3]=='d' && info.Name[4]=='i' && info.Name[5]=='s');
    assert(info.Name[6]=='p' && info.Name[7]==0 && info.Name[260]==0);
    for(unsigned i=0;i<sizeof(info.Name)/sizeof(info.Name[0]);i++)
        if(i>7) assert(info.Name[i]==0);
    return 0;
}
