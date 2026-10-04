struct __attribute__ ((packed)) A { int i; char c; };
struct B { struct A a; } b;
struct A a2;
void bar(int *);
int *g1 = (int*)&b.a.i;
int *g2 = (int*)&b.a;
int *g3 = (int*)&a2.i;
int *g4 = &a2.i;
void f(void){
  int *p;
  p = (int*)&b.a.i;
  p = (int*)&a2.i;
  p = (int*)&b.a;
  bar((int*)&a2.i);
  bar((int*)&b.a.i);
  bar(&a2.i);
  (int*)&a2.i;
}
