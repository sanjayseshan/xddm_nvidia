/* Freestanding memory helpers: XP x86 GDI DLLs must not import NTOSKRNL. */
#include <stddef.h>
void *memcpy(void *dst, const void *src, size_t bytes)
{
    unsigned char *d=dst;
    const unsigned char *s=src;
    size_t i;
    for (i=0;i<bytes;++i) d[i]=s[i];
    return dst;
}
void *memset(void *dst, int value, size_t bytes)
{
    unsigned char *d=dst;
    size_t i;
    for (i=0;i<bytes;++i) d[i]=(unsigned char)value;
    return dst;
}
int memcmp(const void *lhs, const void *rhs, size_t bytes)
{
    const unsigned char *a=lhs, *b=rhs;
    size_t i;
    for(i=0;i<bytes;++i) if(a[i]!=b[i]) return (int)a[i]-(int)b[i];
    return 0;
}
