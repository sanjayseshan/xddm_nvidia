#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <sys/stat.h>

FILE *__acrt_iob_func(unsigned i)
{
    return &__iob_func()[i];
}

int __stdio_common_vsprintf(unsigned long long options, char *str, size_t len, const char *fmt, void *locale, va_list ap)
{
    (void)options; (void)locale;
    if (len == (size_t)-1) return vsprintf(str, fmt, ap);
    return vsnprintf(str, len, fmt, ap);
}

int __stdio_common_vfprintf(unsigned long long options, FILE *f, const char *fmt, void *locale, va_list ap)
{
    (void)options; (void)locale;
    return vfprintf(f, fmt, ap);
}

int __stdio_common_vsscanf(unsigned long long options, const char *str, size_t len, const char *fmt, void *locale, va_list ap)
{
    (void)options; (void)len; (void)locale;
    return vsscanf(str, fmt, ap);
}

short _dclass(double x)
{
    return (short)_fpclass(x);
}

short _fdclass(float x)
{
    return (short)_fpclass((double)x);
}

int _dsign(double x)
{
    return signbit(x) ? 1 : 0;
}

int _fdsign(float x)
{
    return signbit(x) ? 1 : 0;
}

void _assert(const char *msg, const char *file, unsigned line)
{
    fprintf(stderr, "Assertion failed: %s, file %s, line %u\n", msg, file, line);
    abort();
}

size_t strnlen(const char *s, size_t maxlen)
{
    size_t i = 0;
    while (i < maxlen && s[i]) i++;
    return i;
}

int _fstat64i32(int fd, struct _stat64i32 *buf)
{
    return _fstat(fd, (struct _stat *)buf);
}
