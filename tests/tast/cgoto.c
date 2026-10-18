/* T6: computed goto. &&label is a constant void *; goto *p is igoto. */
int disp(int n) {
    static const void *const tab[] = { &&a, &&b, &&c };
    int r = 0;
    goto *tab[n];
a:  r += 1;
b:  r += 2;
c:  return r;
}
int loop(int n) {
    void *p = &&top;
    int s = 0;
top:
    if (n-- > 0) { s++; goto *p; }
    return s;
}
long cast(void) { long v = (long)&&here; here: return v; }
void *pick(int i) { goto *(i ? &&x : &&y); x: return 0; y: return 0; }
