// flags: -Wall
#define AS(...) __attribute__ ((alloc_size (__VA_ARGS__)))
#define AA(p) __attribute__ ((alloc_align (p)))
AS (1) char *f1 (int, int);
AS (1) AS (1) char *f1 (int, int);
AS (2) char *f1 (int, int);
AS (1, 2) char *f1 (int, int);
AS (1) char *f1 (int, int);
AA (1) char *g1 (int, int, int);
AA (2) char *g1 (int, int, int);
AA (3) char *g1 (int, int, int);
AA (1) char *g1 (int, int, int);
char *h1 (int, int);
AS (2) char *h1 (int, int);
AS (2) char *h1 (int, int) { return 0; }
