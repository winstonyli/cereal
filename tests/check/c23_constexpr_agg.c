// flags: -std=c23
struct P { int x, y; float f; double d; char c[3]; int *p; };
constexpr struct P p = { 3, 4, 0.5f, 0.25, "ab", nullptr };
constexpr int arr[4] = { 1, 2, [3] = 9 };
constexpr char str[] = "hey";
constexpr int one = { 1 };
struct Q { struct P in[2]; short s; };
constexpr struct Q q = { { { 1, 2 }, { .y = 7 } }, 5 };
int v1[p.x + p.y];
int v2[arr[3] - arr[1]];
int v3[q.in[1].y + q.s];
_Static_assert(p.x == 3 && p.y == 4, "members");
_Static_assert(p.f == 0.5f && p.d == 0.25, "floats");
_Static_assert(p.c[1] == 'b' && p.c[2] == 0, "chars");
_Static_assert(arr[0] == 1 && arr[1] == 2 && arr[2] == 0 && arr[3] == 9, "array");
_Static_assert(str[0] == 'h' && str[3] == 0, "string");
_Static_assert(one == 1, "braced scalar");
_Static_assert(q.in[0].x == 1 && q.in[1].x == 0 && q.in[1].y == 7, "nested");
_Static_assert(p.p == nullptr, "null member");
int use(void)
{
    constexpr struct P lp = { 5, 6 };
    constexpr int la[2] = { 8, 9 };
    int w[lp.x + la[1]];
    switch (lp.y) { case lp.x + 1: return sizeof w; }
    return (int)&p.x != 0;
}
int ng = 2;
constexpr struct P bad1 = { ng };
constexpr int bad2[2] = { 1, 1.5 };
constexpr struct P bad3 = { 0, 0, 0, 0, "", &ng };
constexpr unsigned char bad4[2] = { 255, 256 };
int out_of_range[arr[4]];
int not_const(int i) { return sizeof(int[arr[i]]); }
void mod(void) { p.x = 1; arr[0]++; (void)&arr[1]; }
