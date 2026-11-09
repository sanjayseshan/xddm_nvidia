/* Freestanding Linux i386 test: no libc can mask recursive helper calls. */
#include <stddef.h>
void *memcpy(void *,const void *,size_t);
void *memset(void *,int,size_t);
int memcmp(const void *,const void *,size_t);
static unsigned char src[264], dst[264];
static int check(void)
{
    size_t n,offset,i;
    for(offset=0;offset<4;++offset) for(n=0;n<=256;++n) {
        for(i=0;i<264;++i) { src[i]=(unsigned char)(i*37); dst[i]=0x55; }
        if(memcpy(dst+offset,src+offset,n)!=dst+offset) return 1;
        for(i=0;i<264;++i)
            if(dst[i] != (i>=offset && i<offset+n ? src[i] : 0x55)) return 2;
        if(memcmp(dst+offset,src+offset,n)!=0) return 3;
        for(i=0;i<n;++i) {
            unsigned char old=dst[offset+i];
            dst[offset+i]=(unsigned char)(old^0x80);
            int expected=(int)dst[offset+i]-(int)src[offset+i];
            if(memcmp(dst+offset,src+offset,n)!=expected) return 4;
            dst[offset+i]=old;
        }
        if(memset(dst+offset,0x1a3,n)!=dst+offset) return 5;
        for(i=0;i<264;++i)
            if(dst[i] != (i>=offset && i<offset+n ? 0xa3 : 0x55)) return 6;
    }
    return 0;
}
#ifdef NVX_RUNTIME_HOSTED
int main(void) { return check(); }
#else
void _start(void)
{
    int result=check();
    __asm__ __volatile__("int $0x80" : : "a"(1), "b"(result) : "memory");
    __builtin_unreachable();
}

#endif
