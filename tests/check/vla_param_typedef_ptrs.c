// flags: -Wvla-parameter
extern int n;
typedef int IA3[3];
typedef int IAn[n];
void a1 (IA3 *x[*]);
void a1 (IA3 *x[n]);
void a2 (int (*x[*])[3]);
void a2 (int (*x[n])[3]);
void a3 (int *x[*]);
void a3 (int *x[n]);
