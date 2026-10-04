#include <xmmintrin.h>
struct s { int i; };
typedef float loc_vector __attribute__((__vector_size__(16)));
typedef __m128 m128_copy;
void g(void)
{
    struct s x;
    __m128 a;
    m128_copy b;
    loc_vector c;
    a = x;
    b = x;
    c = x;
}
