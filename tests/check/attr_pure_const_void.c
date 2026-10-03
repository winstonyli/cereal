// flags: -Wall
void f1 (void) __attribute__ ((const));
void f2 (void) __attribute__ ((pure));
__attribute__ ((const)) void f3 (void);
void f4 (void) __attribute__ ((__pure__, const));
void (*fp) (void) __attribute__ ((const));
void f5 (void) { }
void f5 (void) __attribute__ ((const));
__attribute__ ((pure)) void f6 (void) { }
void *f7 (void) __attribute__ ((const));
int f8 (void) __attribute__ ((pure));
