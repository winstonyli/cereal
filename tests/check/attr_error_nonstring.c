// flags: -Wall
extern void foo (void) __attribute__ ((error (0)));
extern void bar (void) __attribute__ ((warning (0)));
extern void ok1 (void) __attribute__ ((error ("gone")));
extern void ok2 (void) __attribute__ ((warning ("careful")));
int var __attribute__ ((error ("foo")));
