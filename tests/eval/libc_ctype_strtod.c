/* __ctype_b_loc (isalpha, isspace, ...) and strtod: the evaluator's result is compared with gcc's. */
#include <ctype.h>
#include <stdlib.h>

int main(void)
{
    char src[] = "  12.5e1xyz", *end;
    double d = strtod(src, &end);
    int n = 0, c;
    for (c = -1; c < 256; c++)
        n += !!isalpha(c) + 2 * !!isdigit(c) + 4 * !!isspace(c) + 8 * !!ispunct(c) + 16 * !!isxdigit(c) + 32 * !!isupper(c);
    return (n + (int)d + (end - src)) & 0x7f;
}
