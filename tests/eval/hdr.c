/* a static inline function from a system header is retained and run */
#include <endian.h>
#include <stdint.h>

int main(void)
{
    uint32_t a = htole32(0x01020304u);
    uint16_t b = le16toh(0x0506);
    uint64_t c = htole64(0x1122334455667788ull);
    return (int)(a >> 16) + b + (int)(c >> 56);
}
