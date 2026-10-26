/* A pointer the program stores into a drawn object is a pointer, not bytes: zfill reads *buff, and buff can point at the Zio's own z->p, whose bytes are an address (gcc's differ). The sweep must stop there, not report a difference. */
#include <stddef.h>
typedef struct LS { const char *s; size_t size; } LS;
typedef struct Zio { size_t n; const char *p; const char *(*reader)(void *L, void *ud, size_t *sz); void *data; void *L; } Zio;
static const char *getS(void *L, void *ud, size_t *sz)
{
    LS *ls = ud;
    (void)L;
    if (ls->size == 0)
        return 0;
    *sz = ls->size;
    ls->size = 0;
    return ls->s;
}
int zfill(Zio *z)
{
    size_t size;
    const char *buff = z->reader(z->L, z->data, &size);
    if (!buff || size == 0)
        return -1;
    z->n = size - 1;
    z->p = buff;
    return (unsigned char)*(z->p++);
}
int zuse(Zio *z) { z->reader = getS; return zfill(z); }
