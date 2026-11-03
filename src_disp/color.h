#ifndef NVX_COLOR_H
#define NVX_COLOR_H

/* Convert a GDI COLORREF (0x00BBGGRR) to the driver's XRGB pixel word. */
static inline unsigned int NvxColorRefToXrgb(unsigned int color)
{
    return ((color & 0x000000FFU) << 16) |
           (color & 0x0000FF00U) |
           ((color & 0x00FF0000U) >> 16);
}

#endif
