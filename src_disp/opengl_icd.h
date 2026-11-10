#ifndef NVX_OPENGL_ICD_H
#define NVX_OPENGL_ICD_H

#include <stddef.h>
#include <stdint.h>

/* XP's legacy ICD registry/escape interface uses version 2 and a packed
 * driver build version, as do the XP NVIDIA ICD registrations. */
#define NVX_OPENGL_GETINFO_DRVNAME 0U
#define NVX_OPENGL_DRIVER_VERSION 2U
#define NVX_OPENGL_DRIVER_BUILD_VERSION 0x00010000U

typedef struct NVX_OPENGL_DRIVER_INFO {
    uint32_t Version;
    uint32_t DriverVersion;
    uint16_t Name[261];
} NVX_OPENGL_DRIVER_INFO;

static inline int
NvxBuildOpenGlDriverInfo(uint32_t subEscape, void *output,
                         size_t outputBytes)
{
    const size_t legacyBytes = offsetof(NVX_OPENGL_DRIVER_INFO, Name) +
                               256U * sizeof(uint16_t);
    static const uint16_t name[] = {'n','v','x','d','i','s','p',0};
    NVX_OPENGL_DRIVER_INFO *info=(NVX_OPENGL_DRIVER_INFO *)output;
    size_t returnedBytes;
    size_t i;
    if(subEscape!=NVX_OPENGL_GETINFO_DRVNAME||!info||
       outputBytes<legacyBytes) return 0;
    returnedBytes=outputBytes<sizeof(*info)?legacyBytes:sizeof(*info);
    for(i=0;i<returnedBytes;i++) ((uint8_t *)info)[i]=0;
    info->Version=NVX_OPENGL_DRIVER_VERSION;
    info->DriverVersion=NVX_OPENGL_DRIVER_BUILD_VERSION;
    for(i=0;i<sizeof(name)/sizeof(name[0]);i++) info->Name[i]=name[i];
    return (int)returnedBytes;
}

#endif
