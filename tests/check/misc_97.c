#include <stdlib.h>
#include <string.h>
#define R __attribute__((scalar_storage_order("big-endian")))
struct R A { int i; };
void *mk(void);
void *mm(void) __attribute__((malloc));
void *cp(void *, const void *, unsigned long);
extern void *memmove2(void *, const void *, unsigned long) __asm__("memmove");
void f(struct A *a, void *v)
{
  struct A *p1 = mk();
  struct A *p2 = mm();
  struct A *p3 = malloc(4);
  struct A *p4 = (void *)mk();
  struct A *p5 = calloc(1, 4);
  struct A *p6 = realloc(v, 4);
  struct A *p7 = __builtin_alloca(4);
  struct A *p8 = (v);
  struct A *p9 = 1 ? v : v;
  cp(a, a, 4);
  memmove(a, a, 4);
  memmove2(a, a, 4);
  __builtin_memcpy(a, a, 4);
  __builtin_memset(a, 0, 4);
  strcpy(a, "x");
  free(a);
  p1 = memcpy(v, a, 4);
}
