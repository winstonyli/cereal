static const char a[] = "0123456789";
static const char b = a[3];
struct S { const char c[4]; const char d[4]; };
static const struct S e[] = { { "abc", "def" }, { "ghi", "jkl" } };
static const char f = e[1].c[2];
const float fa[2] = { 1.0, 2.0 };
const float fb[2] = { [0] = fa[0], [1] = fa[1] };
int n;
const char bad = a[n];    /* index not constant: error */
