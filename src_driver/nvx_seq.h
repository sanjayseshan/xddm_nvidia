#ifndef NVX_SEQ_H
#define NVX_SEQ_H
#include <stdint.h>
#include <stddef.h>
/* 535 rmgspseq.h: variable-length opcodes 0..8. */
unsigned NvxSeqArgs(uint32_t opcode);
int NvxValidateSeq(const uint32_t *,size_t,uint32_t,uint32_t *);
#endif
