// flags: -Wvla
extern void func (int i, int array[i]);
void g (int n) { int a[n]; (void) a; }
extern int h (int n, int (*)[n]);
void k (int n) { char *p[n]; int é[n]; (void) p; }
const char e0[0] = "";
const char e1[0] = "a";
const char e3[0] = { 0 };
const char e5[0] = { "a" };
const char e6[1] = "";
void f (void) { const char e[0] = ""; (void)(const char[0]){ "" }; }
struct t { int a; char b[0]; };
struct t t0 = { 0, "" };
struct t t1 = { 0, { 0 } };
