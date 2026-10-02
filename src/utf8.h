/* utf8.h - UTF-8 decoding with libcpp's rules (one_utf8_to_cppchar). */
#ifndef CEREAL_UTF8_H
#define CEREAL_UTF8_H

#include <stddef.h>
#include <stdint.h>

/* Decode the sequence at p (n bytes available): its length and *cp, or 0 if
 * it is not valid (a stray continuation byte, a truncated or overlong form, a
 * surrogate).  Like libcpp, accepts the 5- and 6-byte forms. */
static inline int utf8_dec(const unsigned char *p, size_t n, uint32_t *cp)
{
    static const unsigned char masks[6] = {0x7F, 0x1F, 0x0F, 0x07, 0x03, 0x01};
    static const unsigned char patns[6] = {0x00, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC};
    uint32_t c;
    size_t nb, i;
    if (n < 1)
        return 0;
    c = p[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    for (nb = 2; nb < 7; nb++)
        if ((c & ~(uint32_t)masks[nb - 1]) == patns[nb - 1])
            break;
    if (nb == 7 || n < nb)
        return 0;
    c &= masks[nb - 1];
    for (i = 1; i < nb; i++) {
        if ((p[i] & 0xC0) != 0x80)
            return 0;
        c = (c << 6) + (p[i] & 0x3F);
    }
    if ((c <= 0x7F && nb > 1) || (c <= 0x7FF && nb > 2) ||
        (c <= 0xFFFF && nb > 3) || (c <= 0x1FFFFF && nb > 4) ||
        (c <= 0x3FFFFFF && nb > 5))
        return 0;
    if (c > 0x7FFFFFFF || (c >= 0xD800 && c <= 0xDFFF))
        return 0;
    *cp = c;
    return (int)nb;
}

#endif
