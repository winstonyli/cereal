/* the nonnull attribute: argument checks and attribute validation */
extern void f1 (char *, char *, int) __attribute__((nonnull));
extern void f2 (char *, char *) __attribute__((nonnull(1)));
extern void f4 (char *, char *) __attribute__((nonnull(1))) __attribute__((nonnull(2)));
extern void *memcpy (void *, const void *, unsigned long);
void t (int i, char *p, char *q)
{
  f1 (p, q, 0);
  f1 (0, q, i);
  f1 (p, (void *) 0, i);
  f2 (p, 0);
  f2 (0, p);
  f4 (0, p);
  f4 (p, 0);
  f1 (i ? p : 0, q, i);
  f1 (i ? 0 : p, q, i);
  f1 ((i, 0), q, i);
  memcpy (0, p, 1);
  __builtin_strlen (0);
  __builtin_memset (p, 0, 0);
}
extern void e1 () __attribute__((nonnull));
extern void e2 (char *) __attribute__((nonnull(2)));
extern void e3 (char *) __attribute__((nonnull (foo)));
extern void e4 (int) __attribute__((nonnull(1)));
