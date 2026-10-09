void f1(int a) { goto x; typedef int A[a]; A *p[2]; x:; }
void f2(int a) { goto x; typedef int A[a]; A **p; x:; }
void f3(int a) { goto x; typedef int (*B)[a]; B p; x:; }
void f4(int a) { goto x; typedef int (*B)[a]; B *p; x:; }
void f5(int a) { goto x; typedef int A[a]; typedef A *C; C *p; x:; }
void f6(int a) { goto x; int (**p)[a]; x:; }
