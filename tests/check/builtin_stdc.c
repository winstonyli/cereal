enum E { A };
void f(int i, _Bool b, double d, char c, signed char sc, unsigned char uc, enum E e, long l, int *p)
{
    (void)__builtin_stdc_leading_zeros(i);
    (void)__builtin_stdc_leading_zeros(b);
    (void)__builtin_stdc_leading_zeros(d);
    (void)__builtin_stdc_leading_zeros(c);
    (void)__builtin_stdc_leading_zeros(sc);
    (void)__builtin_stdc_leading_zeros(e);
    (void)__builtin_stdc_leading_zeros(p);
    (void)__builtin_stdc_leading_zeros();
    (void)__builtin_stdc_leading_zeros(uc, uc);
    (void)__builtin_stdc_bit_ceil(i);
    (void)__builtin_stdc_has_single_bit(d);
    (void)__builtin_stdc_count_ones(l);
    __typeof__(__builtin_stdc_bit_floor(uc)) t1;
    __typeof__(__builtin_stdc_count_ones(uc)) t2;
    __typeof__(__builtin_stdc_has_single_bit(uc)) t3;
    __typeof__(__builtin_stdc_bit_ceil(1ull)) t4;
    int *x1 = &t1, *x2 = &t2, *x3 = &t3, *x4 = &t4;
}
