#ifndef NVX_RPC_H
#define NVX_RPC_H
#include <stdint.h>
#include <stddef.h>
#define NVX_RPC_SLOT_SIZE 4096U
#define NVX_RPC_HEADER_SIZE 80U
/* 535 GspSystemInfo: natural LP64 layout, ACPI_METHOD_DATA is 484 bytes. */
typedef struct {
    uint64_t gpuPhysAddr,gpuPhysFbAddr,gpuPhysInstAddr,dbdf;
    uint64_t simAccessBuf,atomics,consoleMemSize,maxUserVa;
    uint32_t mirrorBase,mirrorSize;
    uint8_t oorArch;
    uint64_t chipsetProperties;
    uint32_t chipset;
    uint8_t bridge,mnoc,l0s,l1,l1por,mobile,upstream;
    uint16_t fhb[5],chipsetId[5];
    uint32_t acpi[121];
    uint32_t hypervisor;
    uint8_t passthrough;
    uint64_t timerOffset;
    uint32_t totalVfs,firstVf;
    uint64_t vfBar0,vfBar1,vfBar2;
    uint8_t vf64[3];
} NVX_SYSTEM_INFO_535;
int NvxEncodeRpc(uint8_t *,size_t,uint32_t,uint32_t,const void *,size_t);
int NvxDecodeRpc(const uint8_t *,size_t,uint32_t *,uint32_t *,uint32_t *);
#endif
