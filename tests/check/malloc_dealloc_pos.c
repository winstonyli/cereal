// flags: -std=gnu99
void d1 (int, long *);
void d2 (long *, int);
void d3 (void);
void d4 ();
void d5 (int);
long *a1 (int) __attribute__ ((malloc (d1, 2)));
long *a2 (int) __attribute__ ((malloc (d2, 2)));
long *a3 (int) __attribute__ ((malloc (d1, 3)));
long *a4 (int) __attribute__ ((malloc (d1, 0)));
long *a5 (int) __attribute__ ((malloc (d3, 1)));
long *a6 (int) __attribute__ ((malloc (d4, 1)));
long *a7 (int) __attribute__ ((malloc (d5)));
long *a8 (int) __attribute__ ((malloc (d1, 1)));
long *a9 (int) __attribute__ ((malloc (d1, 2, 3)));
long *aa (int) __attribute__ ((malloc (d1, 1.0)));
