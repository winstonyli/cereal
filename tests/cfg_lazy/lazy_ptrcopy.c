/* Part of a pointer the sweep drew is not copied as bytes: pc_run copies 4 bytes of the buffer's own pointer cell (s can alias the buffer itself) and writes the result out; gcc's address bytes differ. The sweep must stop there, not report a difference. */
#include <string.h>
typedef struct Buf { char *b; unsigned long n; } Buf;
long pc_run(Buf *x, const char *s)
{
    memcpy(x->b + x->n, s, 4);
    return x->n;
}
