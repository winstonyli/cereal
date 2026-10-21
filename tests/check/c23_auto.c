// flags: -std=c23
auto a = 1;
auto b = 1.5;
auto c = "s";
auto d = 'x';
auto e = 1ull;
auto f = (char)1;
const int ci = 3;
auto g = ci;
int arr[3];
auto h = arr;
auto i = &arr;
auto j = nullptr;
constexpr auto k = 2;
static auto l = 5;
auto m;
auto n = {1};
auto o = 1, p = 2;
auto *q = &a;
auto r[2] = {1, 2};
auto s(void) { return 1; }
auto t = t;
struct S { int x; };
auto u = (struct S){1};
auto v = a + b;
auto (w) = 1;
auto x = 1, y;
int ff(void) {
    auto x = 1;
    auto y = x + 1.5f;
    auto *p = &x;
    const auto z = 3;
    auto int q = 4;
    register auto rr = 2;
    auto bf = (_Bool)1;
    typeof(x) tx = 0;
    auto vl = (int (*)[x])0;
    auto fp = ff;
    { auto x = x; }
    return x + y + z + q + rr + bf + tx;
}
static auto s1 = 1;
auto static s2 = 2;
extern auto s3 = 3;
_Thread_local auto s6 = 6;
auto _Thread_local s7 = 7;
auto int s8 = 8;
auto long s9 = 9;
static auto int s10 = 10;
auto auto s11 = 11;
auto const s12 = 12;
const auto s13 = 13;
auto volatile s14 = 14;
auto unsigned s15 = 15;
auto s16 = 16, *s17 = 0;
void f2(void) {
    static auto a1 = 1;
    auto static a2 = 2;
    extern auto a3 = 3;
    register auto a4 = 4;
    auto auto a5 = 5;
    auto a6 = 6;
    auto int a7;
    auto a8;
    typeof(a6) a9 = a1;
    auto a10 = a1 + a2;
    auto a11 = (long)a6;
}
