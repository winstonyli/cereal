// flags: -Wall -Warray-parameter=2 -Wvla-parameter
void f1 (void (*(*(*[6])[5])(void))(void));
void f1 (void (*(*(*[7])[5])(void))(void));
void f1 (void (*(*(**)[5])(void))(void));
int n1, n2;
void f2 (int *(*(*[2])[n1])[1]);
void f2 (int *(*(*[2])[1])[n2]);
void f3 (int (*(*(*)[2])[n1])[1]);
void f3 (int (*(*(*)[2])[1])[n2]);
