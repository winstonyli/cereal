// T1 statements: if, loops, switch, goto, labels, calls, return.
int g(int);
enum E { A, B };
int sw(char c, enum E e) {
    switch (c) { case 1: return 1; case 'a': return 2; }
    switch (e) { case A: return 3; case B: return 4; default: break; }
    return 0;
}
int cl(short s) { return g(s) + g(2); }
int jmp(int n) {
    int i = 0;
top:
    i++;
    if (i < n) goto top;
    do { n--; } while (n > 0);
    while (n) { if (n) continue; break; }
    return i;
}
int forv(int n) { int s = 0; for (;;) { s++; if (s > n) break; } for (int i = 0; i < n; i++) s += i; return s; }
void nothing(void) { return; }
