// flags: -Wall
typedef __SIZE_TYPE__ size_t;
extern void *memcpy (void *restrict, const void *restrict, size_t);
extern void *mempcpy (void *restrict, const void *restrict, size_t);
extern char *strncpy (char *restrict, const char *restrict, size_t);
extern char *strcat (char *restrict, const char *restrict);
void user (char *restrict, const char *restrict);
struct S { char a[4]; char *p; } s;
char arr[8], brr[8];
void f (char *p, char *q, struct S *r, size_t n)
{
  memcpy (p, p, 1);
  memcpy (p, p, 0);
  mempcpy (p, p, 0);
  strncpy (p, p, 0);
  memcpy (p, p + 0, n);
  memcpy (p, (char *)p, n);
  memcpy (p, q, 1);
  memcpy (p, p + 1, 1);
  memcpy (arr, arr, 1);
  memcpy (arr, brr, 1);
  memcpy (r->p, r->p, 1);
  memcpy (r->p, &r->p[0], 1);
  memcpy (r->a, &r->a[0], 1);
  strcat (r->a, r->a);
  user (p, p); user (p, p);
  user (p, q);
}
