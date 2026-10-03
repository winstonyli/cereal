// flags: -Wall
int f(int a) { lab: if (&&lab) a++; int b = !&&lab; return a == 0 ? b : (&&lab == 0); }
int l_f(int a) { lb: if (&&lb) a++; return a; }
int l_g(int a) { lb: while (&&lb) a++; return a; }
int l_h(int a) { lb: return &&lb ? a : 1; }
int l_k(int a) { lb: if (!&&lb) a++; return a; }
_Complex float cf, cfa[2];
void T(int);
void cplx(void) {
  T(&__real__ cf == 0);
  T(0 != &__imag__ cf);
  T(&__real__ cfa[0] == 0);
}
