#ifndef NVX_RPC_H
#define NVX_RPC_H
#include <stdint.h>
#include <stddef.h>
#define NVX_RPC_SLOT_SIZE 4096U
#define NVX_RPC_HEADER_SIZE 80U
int NvxEncodeRpc(uint8_t *,size_t,uint32_t,uint32_t,const void *,size_t);
int NvxDecodeRpc(const uint8_t *,size_t,uint32_t *,uint32_t *,uint32_t *);
#endif
