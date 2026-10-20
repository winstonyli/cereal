/* __builtin_{add,sub,mul}_overflow for tests/asm_exec.py: operand and result types of every
 * width and signedness the back end takes (an unsigned 64-bit operand with a signed one, or
 * into a signed result, is reported unsupported), and the typed s/u forms. */
#define OV3(N, A, B, R, F)                                                    \
long long N##_##F(A a, B b)                                                   \
{                                                                             \
    R r = 3;                                                                  \
    int o = __builtin_##F##_overflow(a, b, &r);                               \
    return (long long)r * 2 + o;                                              \
}
#define OV(N, A, B, R) OV3(N, A, B, R, add) OV3(N, A, B, R, sub) OV3(N, A, B, R, mul)
#define OVR(N, A, B)                                                          \
    OV(N##_i, A, B, int) OV(N##_u, A, B, unsigned) OV(N##_l, A, B, long long)  \
    OV(N##_ul, A, B, unsigned long long) OV(N##_s, A, B, short)               \
    OV(N##_uc, A, B, unsigned char)

OVR(ii, int, int)
OVR(uu, unsigned, unsigned)
OVR(ll, long long, long long)
OVR(ulul, unsigned long long, unsigned long long)
OVR(cc, signed char, signed char)
OVR(iu, int, unsigned)
OVR(ul, unsigned, long long)
OVR(us, unsigned short, short)

long long t_sadd(int a, int b) { int r; int o = __builtin_sadd_overflow(a, b, &r); return (long long)r * 2 + o; }
long long t_ssub(int a, int b) { int r; int o = __builtin_ssub_overflow(a, b, &r); return (long long)r * 2 + o; }
long long t_smul(int a, int b) { int r; int o = __builtin_smul_overflow(a, b, &r); return (long long)r * 2 + o; }
long long t_uadd(unsigned a, unsigned b) { unsigned r; int o = __builtin_uadd_overflow(a, b, &r); return (long long)r * 2 + o; }
long long t_usub(unsigned a, unsigned b) { unsigned r; int o = __builtin_usub_overflow(a, b, &r); return (long long)r * 2 + o; }
long long t_umul(unsigned a, unsigned b) { unsigned r; int o = __builtin_umul_overflow(a, b, &r); return (long long)r * 2 + o; }
long long t_saddl(long a, long b) { long r; int o = __builtin_saddl_overflow(a, b, &r); return r * 2 + o; }
long long t_smulll(long long a, long long b) { long long r; int o = __builtin_smulll_overflow(a, b, &r); return r * 2 + o; }
long long t_umulll(unsigned long long a, unsigned long long b)
{
    unsigned long long r;
    int o = __builtin_umulll_overflow(a, b, &r);
    return (long long)r * 2 + o;
}
long long t_conv(long long a, long long b)  /* the typed form converts its operands to int first */
{
    int r;
    int o = __builtin_sadd_overflow(a, b, &r);
    return (long long)r * 2 + o;
}
int t_cond(int a, int b) { int r; return __builtin_add_overflow(a, b, &r) ? -1 : r; }
