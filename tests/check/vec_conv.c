typedef char vc __attribute__((vector_size(8)));
typedef short vs __attribute__((vector_size(8)));
typedef int vi __attribute__((vector_size(16)));
typedef unsigned vu __attribute__((vector_size(16)));
vs q;
vc y = (vs){1,2,3,4};
vc z = q;
vi a;
vu b;
void f(void)
{
    vc m = q;
    vu ok = a < b;      /* a comparison is opaque */
    vu bad = a;
    m = q;
    (void)ok; (void)bad; (void)m;
}
