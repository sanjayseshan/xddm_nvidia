#ifndef NVX_FW_H
#define NVX_FW_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t DataOffset, DataSize;
    uint32_t ImemOffset, ImemSize, DmemOffset, DmemSize;
    uint32_t PatchOffset, SignatureOffset, SignatureSize, SignatureCount;
    uint32_t FuseVersion, EngineId, UcodeId;
} NVX_BOOTER_INFO;

int NvxParseBooter(const unsigned char *data, size_t size, NVX_BOOTER_INFO *info);
int NvxFindElfSection(const unsigned char *data, size_t size, const char *name,
                      uint32_t *offset, uint32_t *length);
int NvxSelectSignature(uint32_t fuse, uint32_t version, uint32_t count);
#endif
