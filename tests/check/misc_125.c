// flags: -Wduplicate-decl-specifier
void f(void){ char *restrict restrict r4; char *const const c; int *volatile volatile v; }
void r4(char *restrict restrict t) {}
void g(int a[const const 3], int b[static restrict restrict 2]);
int * const volatile const q;
typedef int *restrict restrict IRR;
#define RT2 char *restrict
typedef char *restrict RT1;
void m(void) { RT1 restrict r1; RT2 restrict r2; RT2 restrict restrict r3; }
