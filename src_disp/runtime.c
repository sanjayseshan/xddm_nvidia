/* Freestanding x86 helpers: XP GDI DLLs must not import NTOSKRNL.
 * Presentation compares full scanout rows; use block operations rather than
 * byte loops on that hot path. These instructions require no SIMD state. */
#include <stddef.h>
void *memcpy(void *dst, const void *src, size_t bytes)
{
    void *result=dst;
    size_t words=bytes/4, tail=bytes%4;
    __asm__ __volatile__("cld; rep movsl"
        : "+D"(dst), "+S"(src), "+c"(words) : : "memory", "cc");
    __asm__ __volatile__("rep movsb"
        : "+D"(dst), "+S"(src), "+c"(tail) : : "memory", "cc");
    return result;
}
void *memset(void *dst, int value, size_t bytes)
{
    void *result=dst;
    unsigned int pattern=(unsigned char)value * 0x01010101U;
    size_t words=bytes/4, tail=bytes%4;
    __asm__ __volatile__("cld; rep stosl"
        : "+D"(dst), "+c"(words) : "a"(pattern) : "memory", "cc");
    __asm__ __volatile__("rep stosb"
        : "+D"(dst), "+c"(tail) : "a"(pattern) : "memory", "cc");
    return result;
}
/* Packed aliases permit unaligned dword loads without alignment or aliasing UB.
 * A 16-byte equality loop avoids REP CMPS microcode on the common unchanged
 * screen-row case, while byte fallback preserves memcmp ordering exactly. */
typedef struct __attribute__((packed, may_alias)) { unsigned int value; } NVX_WORD;
static unsigned int NvxReadWord(const unsigned char *p)
{
    return ((const NVX_WORD *)p)->value;
}
int memcmp(const void *lhs, const void *rhs, size_t bytes)
{
    const unsigned char *a=lhs, *b=rhs;
    size_t i;
    while(bytes>=16) {
        unsigned int diff=(NvxReadWord(a)^NvxReadWord(b)) |
            (NvxReadWord(a+4)^NvxReadWord(b+4)) |
            (NvxReadWord(a+8)^NvxReadWord(b+8)) |
            (NvxReadWord(a+12)^NvxReadWord(b+12));
        if(diff) {
            for(i=0;i<16;++i) if(a[i]!=b[i]) return (int)a[i]-(int)b[i];
        }
        a+=16; b+=16; bytes-=16;
    }
    for(i=0;i<bytes;++i) if(a[i]!=b[i]) return (int)a[i]-(int)b[i];
    return 0;
}
