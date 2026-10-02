// flags: -std=c99
struct s { int b[1]; const int e[1]; int c[2][3]; const int f[2][3]; };
const struct s cv; struct s v; struct s *pv; const struct s *pcv;
const int a[3]; const int a2[2][3];
void f(void) {
  *a = 1; **a2 = 1; *a2[1] = 1;
  *v.e = 1; *cv.b = 1; *pcv->b = 1; *pv->e = 1; *pcv->c[1] = 1; **pcv->c = 1;
  *cv.e = 1; **cv.f = 1; *cv.f[1] = 1; *cv.c[1] = 1;
  cv.b[0] = 1; cv.c[1][2] = 1;
}
