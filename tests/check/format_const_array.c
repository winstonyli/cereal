// flags: -Wall -Wformat=2
int printf(const char *, ...);
const char g[] = "%d %d";
const char h[3] = "%d";
char *p;
void f(void) {
  const char l[] = "%s";
  const char q[] = "";
  const signed char sc[] = "%d";
  printf((const char *)l, 1);
  printf((const char *)(l), 1);
  printf(g, 1);
  printf(g + 3, 1.0);
  printf(l + 1, 1);
  printf(q, 1);
  printf(h, 1.0);
  printf((char *)sc, 1);
  printf("%d" + 1);
}
void g2(void) { const char g[] = "%f"; printf(g, 1); }
