// flags: -Wduplicated-branches
extern int g, a[10];
extern void foo (int);
struct S { int x; } s;
#define XMEM(R) ((R).x)
#define XSTR(R) ((R).x)
#define A g = 1
#define B g = 1
int f (int i, int *p)
{
  if (i == 0) g = i * 6; else g = 6 * i;       /* warn: commutative */
  if (i == 1) g = i / 6; else g = 6 / i;       /* no */
  if (i == 2) { p++; return *p; } else { p++; return *p; }   /* warn */
  if (i == 3) return *++p; else return ++*p;   /* no */
  if (i == 4) p += (g + (1 + 2)); else p += (g + (1 + 1 + 1));  /* warn */
  if (i == 5) { { g++; } } else { { g++; } }   /* warn: nested blocks */
  if (i == 6) ; else ;                         /* no: empty */
  if (i == 7) XMEM(s) = 1; else XSTR(s) = 1;   /* no: different macros */
  if (i == 8) s.x = 1; else s.x = 1;           /* warn */
  if (i == 9) A; else B;                       /* no */
  if (i == 10) g = (unsigned char) i; else g = (signed char) i;  /* no */
  if (i == 11) { g = 1; foo (2); } else { foo (2); g = 1; }      /* no */
  if (i == 12) g = a[i]; else if (i == 13) g = 2; else g = 2;    /* warn inner */
  *p += i ? 1 : 1;                             /* warn */
  *p += i ? i++ : i++;                         /* warn, deferred */
  *p += i ? a[i] : a[i + 1];                   /* no */
  return i ? ({ foo (i); 1; }) : ({ foo (i); 1; });  /* warn */
}
int h = 1 ? 2 : 2;
