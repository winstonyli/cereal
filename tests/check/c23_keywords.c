// flags: -std=c23
static_assert(sizeof(int) == 4);
static_assert(1, "msg");
thread_local int tl;
bool b = true;
bool f = false;
int n = 1'000'000 + 0x1'F + 0b1'0 + 0'7;
double d = 1'0.2'5 + 1e1'0;
unsigned char u = u8'a';
typeof(n) m;
typeof_unqual(const int) k;
alignas(16) char buf[16];
int al = alignof(double);
static_assert(true && !false);
static_assert(__STDC_VERSION__ == 202311L);
static_assert(_Generic(true, bool: 1));
static_assert(_Generic(u8'a', unsigned char: 1));
static_assert(1'0 == 10 && 0x1'0 == 16);
static_assert(sizeof(typeof_unqual(const int)) == 4);
static_assert(_Generic((typeof_unqual(const int)) 0, int: 1));
static_assert(__builtin_types_compatible_p(typeof(1'0), int));
[[deprecated]] int dp;
int va(...)
{
    __builtin_va_list ap;
    __builtin_c23_va_start(ap);
    __builtin_va_end(ap);
    return 0;
}
