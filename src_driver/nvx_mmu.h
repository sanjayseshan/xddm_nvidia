#ifndef NVX_MMU_H
#define NVX_MMU_H
#include <stdint.h>
typedef int (*NVX_MMU_READ64)(void *,uint64_t,uint64_t *);
typedef struct {
    uint64_t physical;
    uint32_t pageShift,kind,writable;
} NVX_MMU_MAPPING;
enum { NVX_MMU_OK=1,NVX_MMU_INVALID=0,NVX_MMU_ABSENT=-1,
       NVX_MMU_APERTURE=-2,NVX_MMU_KIND=-3,NVX_MMU_IO=-4,NVX_MMU_RANGE=-5 };
/* Pascal+ VER2 tables; only linear VRAM leaves and VRAM table pointers. */
int NvxMmuTranslateVram(NVX_MMU_READ64,void *,uint64_t,uint64_t,uint64_t,
                        unsigned,NVX_MMU_MAPPING *);
#endif
