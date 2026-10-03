// flags: -Wduplicated-branches
extern void foo(int);
extern int g;
void f(int i, int *p) {
  *p += i ? ({ i; }) : ({ i; });
  *p += i ? ({ g; }) : ({ g; });
  *p += i ? ({ i + 1; }) : ({ i + 1; });
  *p += i ? ({ foo (i); i++; }) : ({ foo (i); i++; });
  if (i) ({ i; }); else ({ i; });
  if (i) ({ foo (1); foo (2); }); else ({ foo (1); foo (2); });
  if (i) ({ 1; }); else ({ 1; });
  if (i) ({ i++; i++; }); else ({ i++; i++; });
}
