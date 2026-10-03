// flags: -Wsizeof-pointer-memaccess -Wno-discarded-qualifiers -Wno-incompatible-pointer-types -Wno-unused-value
typedef __SIZE_TYPE__ size_t;
extern void *memset (void *, int, size_t);
extern void *memcpy (void *, const void *, size_t);
extern void *mempcpy (void *, const void *, size_t);
extern void *memmove (void *, const void *, size_t);
extern int memcmp (const void *, const void *, size_t);
extern int bcmp (const void *, const void *, size_t);
extern void bzero (void *, size_t);
extern void bcopy (const void *, void *, size_t);
extern char *strncpy (char *, const char *, size_t);
extern int strncmp (const char *, const char *, size_t);
extern int snprintf (char *, size_t, const char *, ...);
extern void *memchr (const void *, int, size_t);
struct A { int a; };

void f1 (int *p, int *q, void *v, const char *cq, char *c, struct A *pa,
         char a10[10], unsigned char *u, int **pp, struct A s)
{
  char arr[10]; const int *cp = p;
  memset (p, 0, sizeof q);
  memset (v, 0, sizeof v);
  memset (p, 0, (sizeof (p)));
  memset (p, 0, (size_t) sizeof (p));
  memset (p, 0, sizeof (p) * 1);
  memcpy (p, q, sizeof p);
  memcpy (p, q, sizeof q);
  memcpy (p, cq, sizeof cq);
  memcpy (cp, q, sizeof cp);
  memcpy (p, cp, sizeof (const int *));
  memcpy (p, cp, sizeof (int *));
  memcmp (p, q, sizeof p);
  bcmp (p, q, sizeof q);
  bzero (p, sizeof p);
  bzero (p, sizeof (int *));
  bcopy (p, q, sizeof p);
  bcopy (p, q, sizeof q);
  strncmp (c, cq, sizeof c);
  strncmp (c, cq, sizeof cq);
  strncpy (c, cq, sizeof cq);
  strncpy (arr, cq, sizeof arr);
  strncpy (arr, arr, sizeof arr);
  strncpy (c, arr, sizeof arr);
  strncpy (c, "abc", sizeof "abc");
  strncpy (pa, cq, sizeof pa);
  strncpy ((char *) p, cq, sizeof p);
  snprintf (c, sizeof c, "x");
  snprintf (p, sizeof p, "x");
  snprintf (arr, sizeof arr, "x");
  snprintf (c, sizeof (char *), "x");
  memchr (a10, 0, sizeof a10);
  memchr (u, 0, sizeof u);
  memset (pp, 0, sizeof pp);
  memset (&p, 0, sizeof &p);
  memset (&s, 0, sizeof &s);
  memset (&s, 0, sizeof (&s));
  memset (pa, 0, sizeof pa);
  memset (v, 0, sizeof (void *));
  memset (&arr[0], 0, sizeof &arr[0]);
  memset (arr, 0, sizeof arr[0]);
  memset (arr, 0, sizeof (arr + 1));
  memset (arr + 1, 0, sizeof (arr + 1));
  memset (p + 1, 0, sizeof (p + 1));
  memset (p, 0, sizeof (p) + sizeof (q));
}

void f2 (void *v, const void *cv, struct A *pa, struct A *pb, int *p,
         const int *cp, char *c)
{
  memcpy (v, cv, sizeof cv);
  mempcpy (v, pa, sizeof pa);
  memcpy (v, pa, sizeof pa);
  memmove (v, pa, sizeof pa);
  memcpy (pb, pa, sizeof pa);
  memcpy (pb, pa, sizeof (struct A *));
  memcmp (pa, pb, sizeof pb);
  memcmp (pa, p, sizeof p);
  memcmp (p, pa, sizeof p);
  memcmp (v, cv, sizeof (void *));
  memchr (pa, 0, sizeof (struct A *));
  memcpy (p, cp, sizeof cp);
  memcpy (p, cp, sizeof (const int *));
  memcpy (v, cp, sizeof (const int *));
  memcpy (c, "abc", sizeof "abc");
  memcpy (c, c, sizeof c);
  strncpy (c, c, sizeof c);
  strncpy (c, "abc", sizeof ("abc"));
  strncpy (c, cp, sizeof cp);
  strncpy (c, (const char *) cp, sizeof cp);
}

void f3 (char *y, char *z, int *q, void *x)
{
  char *y3; char buf[8];
  memcpy (&y3, y, sizeof y3);
  memcpy (y, &y3, sizeof y3);
  memcpy (y, z, sizeof y3);
  memcpy (y, buf, sizeof y3);
  memset (y, 0, sizeof y3);
  memcpy (q, &y3, sizeof y3);
  memcpy ((char *) &y3, y, sizeof y3);
  memcpy (&y3, y, sizeof (char *));
  memcpy (y, &y3, sizeof (char *));
  memcpy (x, y, sizeof y3);
  memcpy (x, &y3, sizeof y3);
  memcpy (z, y, sizeof y3);
  memcpy (q, y, sizeof y3);
  memcpy (q, z, sizeof y3);
  memcpy (x, z, sizeof y3);
  __builtin___memset_chk (y, 0, sizeof y, __builtin_object_size (y, 1));
  __builtin___strncpy_chk (y, z, sizeof z, 8);
  __builtin_memset (y, 0, sizeof (y));
  __builtin_strncat (y, z, sizeof (y));
}
