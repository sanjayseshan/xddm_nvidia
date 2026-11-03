#include <assert.h>
#include "../color.h"

int main(void)
{
    assert(NvxColorRefToXrgb(0x00000000U) == 0x00000000U);
    assert(NvxColorRefToXrgb(0x000000FFU) == 0x00FF0000U); /* red */
    assert(NvxColorRefToXrgb(0x0000FF00U) == 0x0000FF00U); /* green */
    assert(NvxColorRefToXrgb(0x00FF0000U) == 0x000000FFU); /* blue */
    assert(NvxColorRefToXrgb(0x00D08410U) == 0x001084D0U);
    return 0;
}
