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
    uint32_t PkcDataOffset;
    uint32_t ImemPhysBase;
    uint32_t ImemVirtBase;
    uint32_t DmemPhysBase;
    uint32_t DmemImageOffset;
    uint32_t InterfaceImageOffset;
    uint32_t SignaturesOffset;
    uint32_t SignaturesSize;
    uint32_t DmemMapperOffset;
    uint32_t DmemMapperSignature;
    uint32_t CommandInOffset;
    uint32_t CommandInSize;
    uint32_t CommandOutOffset;
    uint32_t CommandOutSize;
    uint32_t ImageCrc32;
    uint32_t SignaturesCrc32;
    uint16_t EngineIdMask;
    uint16_t SignatureVersions;
    uint16_t DmemMapperVersion;
    uint16_t DmemMapperSize;
    uint8_t DescriptorVersion;
    uint8_t ApplicationId;
    uint8_t TargetId;
    uint8_t UcodeId;
    uint8_t SignatureCount;
    uint8_t InterfaceVersion;
    uint8_t InterfaceHeaderSize;
    uint8_t InterfaceEntrySize;
    uint8_t InterfaceEntryCount;
    uint8_t ValidationStage;
    uint8_t CommandOutValid;
} NVX_FWSEC_INFO;

typedef struct _NVX_FRTS_LAYOUT {
    uint64_t FbSize;
    uint64_t VgaWorkspaceOffset;
    uint64_t MmuLockLo;
    uint64_t MmuLockHi;
    uint64_t VbiosReservedOffset;
    uint64_t GspFwWprEnd;
    uint64_t FrtsOffset;
    uint32_t FrtsSize4k;
    uint8_t MmuLockValid;
} NVX_FRTS_LAYOUT;

enum {
    NVX_VBIOS_OK = 0,
    NVX_VBIOS_IO_ERROR = -1,
    NVX_VBIOS_INVALID_ROM = -2,
    NVX_VBIOS_NO_BIT = -3,
    NVX_VBIOS_NO_FWSEC = -4,
    NVX_VBIOS_BAD_DESCRIPTOR = -5,
    NVX_VBIOS_BAD_INTERFACE = -6
};

int NvxParseFwsecRom(NVX_ROM_READ32 read32, void *context,
                     uint32_t maxSize, NVX_FWSEC_INFO *info);
int NvxSelectFwsecSignature(const NVX_FWSEC_INFO *info, uint32_t rawFuse,
                            uint32_t *fuseVersion, uint32_t *signatureIndex,
                            uint32_t *signatureOffset);
int NvxComputeFrtsLayout(uint64_t fbSize, uint32_t workspaceReg,
                         uint32_t mmuLockPlm, uint32_t mmuLockLoReg,
                         uint32_t mmuLockHiReg, NVX_FRTS_LAYOUT *layout);
int NvxValidateFrtsWpr2(uint64_t frtsOffset, uint32_t scratch,
                         uint32_t wprLo, uint32_t wprHi);

#endif
