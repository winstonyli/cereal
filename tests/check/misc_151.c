// flags: -Wall
typedef _Complex float Cflt;
void T(int);
void t(Cflt *p, int i, Cflt cf[], Cflt q[][2]) {
  T (0 == &__real__ p[i]);
  T (0 == &__imag__ p[3]);
  T (0 == &__real__ *p);
  T (&__real__ q[i][i] == 0);
  T (0 == &p[i]);
}
