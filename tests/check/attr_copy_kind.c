#define ATTR(list) __attribute__ (list)
void ref0 (void);
ATTR ((copy (ref0))) void f0 (void);
ATTR ((copy (&ref0))) void f1 (void);
ATTR ((copy (*ref0))) void f2 (void);
int v0;
ATTR ((copy (v0))) void
f3 (void);
void f4 (void);
ATTR ((copy (f4))) int
v1;
ATTR ((copy (v0 + 1)))
void f5 (void);
void xref1 (void);
ATTR ((copy (xref1))) void
xref1 (void);
ATTR ((copy (xref1), copy (xref1))) void
xref1 (void);
