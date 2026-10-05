// flags: -Wconversion
struct S{unsigned u:3; int i:5; long long l:40; unsigned w:32;} s; double d; float fl;
void f(void){ unsigned u=1.5; s.u=1.5; s.i=-1.5; s.l=2.5; s.u=d; s.i=fl; s.u=9.5; s.w=1.5; s.i=100.5; }
