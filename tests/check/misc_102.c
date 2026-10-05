/* A built-in used as a function pointer is named in the warning: __builtin_X,
 * and a library built-in redeclared without a prototype (which keeps the
 * built-in's own type).  A prototyped redeclaration is an ordinary function. */
typedef unsigned long size_t;
typedef void *(memcpy_t)(void *, const void *, size_t);
typedef void *(memset_t)(void *, int, size_t);

void *memset();
void *memcpy();
void *memmove(void *, const void *, size_t);

void take_memcpy(memcpy_t *);
void take_any(int, ...);

void init(void)
{
    memset_t *ok = memset;
    memcpy_t *bad = memset;
    memcpy_t *plain = memmove;
    memset_t *bad2 = memmove;
    _Bool (*abs_f)(int) = __builtin_abs;
}

void assign(void)
{
    memcpy_t *p;
    p = memset;
    p = (memset);
    p = &memset;
}

memset_t *args(int i)
{
    take_memcpy(memset);
    take_any(0, i ? memcpy : memset);
    take_any(0, i ? memcpy : memcpy);
    return memcpy;
}

int (*ret(void))(void)
{
    return __builtin_abs;
}

void cond(int i)
{
    void *v = i ? __builtin_abs : __builtin_strlen;
    int (*q)(void) = i ? __builtin_abs : 0;
}
