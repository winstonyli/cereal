// flags: -std=gnu99
#define ALIGN(n) __attribute__((aligned(n)))
#define Y(e, s, a) _Static_assert(__builtin_has_attribute(s, a) == e, #s ", " #a)
int ALIGN(8) v8;
void *f(void *, void *) __attribute__((nonnull(2)));
typedef struct { char c[16]; } S16;
typedef S16 ALIGN(2) ALIGN(8) ALIGN(16) T16;
typedef T16 *PT;
typedef T16 AT[3];
typedef AT AAT[2];
typedef void FN(void *, void *);
typedef FN __attribute__((nonnull)) FNN;
typedef void __attribute__((nonnull(2))) FN2(void *, void *);
void t(void)
{
  Y(1, v8, aligned(8));
  Y(0, v8, aligned(4));
  Y(1, T16, aligned(16));
  Y(0, T16, aligned(8));      /* a typedef keeps only its largest */
  Y(0, PT, aligned);          /* pointer to an aligned type */
  Y(1, AAT, aligned(16));     /* user alignment propagates to arrays */
  Y(0, AAT, aligned(8));
  Y(1, f, nonnull(2));
  Y(0, f, nonnull(1));
  Y(1, FNN, nonnull(1));
  Y(0, FN2, nonnull);
  Y(1, FN2, nonnull(2));
  Y(1, f, nonnull(1));        /* wrong: fails */
}
