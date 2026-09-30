static int a;
extern int a;
int a;
extern int b;
static int b;
static void f(void);
void f(void);
void g(void);
static void g(void);
int h = 1;
int h = 2;
void k(void) { int q; int q; }
void m(int p) { int p; }
void n(void) { static int r; extern int r; }
void o(void) { int s; { extern int s; } }
