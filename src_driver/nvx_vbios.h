#ifndef NVX_VBIOS_H
#define NVX_VBIOS_H

#include <stdint.h>

typedef int (*NVX_ROM_READ32)(void *context, uint32_t offset, uint32_t *value);

typedef struct _NVX_FWSEC_INFO {
    uint32_t BiosSize;
    uint32_t ExpansionRomOffset;
    uint32_t BitOffset;
    uint64_t VbiosVersion;
    uint32_t FalconTableOffset;
    uint32_t DescriptorOffset;
    uint32_t DescriptorSize;
    uint32_t ImageOffset;
    uint32_t StoredSize;
    uint32_t ImemLoadSize;
    uint32_t DmemLoadSize;
    uint32_t InterfaceOffset;
    uint16_t EngineIdMask;
    uint16_t SignatureVersions;
    uint8_t DescriptorVersion;
    uint8_t ApplicationId;
    uint8_t TargetId;
    uint8_t UcodeId;
    uint8_t SignatureCount;
} NVX_FWSEC_INFO;

enum {
    NVX_VBIOS_OK = 0,
    NVX_VBIOS_IO_ERROR = -1,
    NVX_VBIOS_INVALID_ROM = -2,
    NVX_VBIOS_NO_BIT = -3,
    NVX_VBIOS_NO_FWSEC = -4,
    NVX_VBIOS_BAD_DESCRIPTOR = -5
};

int NvxParseFwsecRom(NVX_ROM_READ32 read32, void *context,
                     uint32_t maxSize, NVX_FWSEC_INFO *info);

#endif
