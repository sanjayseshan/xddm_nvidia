#ifndef NVX_SEQ_H
#define NVX_SEQ_H
#include <stdint.h>
#include <stddef.h>
/* 535 rmgspseq.h: variable-length opcodes 0..8. */
unsigned NvxSeqArgs(uint32_t opcode);
int NvxValidateSeq(const uint32_t *,size_t,uint32_t,uint32_t *);
typedef struct {
    void *context;
    int (*read)(void *,uint32_t,uint32_t *);
    int (*write)(void *,uint32_t,uint32_t);
    int (*delay)(void *,uint32_t);
    int (*core)(void *,uint32_t);
} NVX_SEQ_IO;
int NvxExecuteSeq(uint32_t *,size_t,uint32_t,const NVX_SEQ_IO *,uint32_t *);
#endif
