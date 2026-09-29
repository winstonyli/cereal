/* GNU extensions and the C11 keywords glibc headers use. */
__extension__ typedef long long ll;
typedef __typeof__(sizeof 0) size_t;
typedef __builtin_va_list va_list;
extern int printf(const char *, ...) __attribute__((__format__(__printf__, 1, 2), nonnull (1)));
extern int renamed(void) __asm__("real_name");
static int counter __attribute__((unused)) = 0;
struct __attribute__((packed)) P { char c; int i; } __attribute__((aligned(8)));
struct F { int n; struct { int a, b; }; union { int u; float f; }; int tail[]; };
_Static_assert(sizeof(int) >= 2, "int");
_Alignas(16) static char buf[32];
_Atomic int at;
_Atomic(long) at2;
_Noreturn void die(void);
__thread int tls;
_Complex double z;
__int128 big;
int kr(a, b) int a; char *b; { return a + *b; }
double kr_decl(x) __attribute__((const));   /* a declaration, not a K&R definition */
static inline __attribute__((always_inline)) int sq(int x) { return x * x; }
struct pt { int x, y; };
struct pt old_style = { x: 1, y: 2 };
int ranges[8] = { [0 ... 3] = 1, [4] 2 };
int generic = _Generic(1.0, int: 1, double: 2, default: 3);
unsigned long off = __builtin_offsetof(struct F, tail[2]);
int compat = __builtin_types_compatible_p(int, long);

void gnu(int n, ...)
{
    __label__ done;
    va_list ap;
    int x = ({ int t = n; t * 2; });
    __typeof__(x) y = x ?: 1;
    static void *targets[] = { &&done };
    int nested(int k) { return k + n; }
    __builtin_va_start(ap, n);
    y += __builtin_va_arg(ap, int);
    __builtin_va_end(ap);
    switch (n) {
    case 1 ... 5:
        __attribute__((fallthrough));
    case 6:
        y = nested(y);
        break;
    default:;
    }
    __asm__ __volatile__("" : "=r"(x) : "0"(y) : "memory");
    __asm__ goto("" :::: done);
    x = __extension__ 0b101;
    __real__ z = 1.0;
    y = (int)__imag__ z + __alignof__(double) + _Alignof(int);
    goto *targets[0];
done:
    (void)x;
}
