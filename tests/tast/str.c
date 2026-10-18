/* TAST T2: string literals used as values. */
const char *s1(void) { return "abc"; }
char c1(void) { return "abc"[1]; }
char c2(int i) { return *("xyz" + i); }
int sz(void) { return sizeof "abcd"; }
const char *tern(int c) { return c ? "yes" : "no"; }
void sink(const char *);
void call(void) { sink("hello"); sink("a" "b"); }
const int *wide(void) { return (const int *)L"w"; }
