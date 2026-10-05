// flags: -std=gnu99
/* An element of a string literal is read-only, with a warning; a conditional
 * index prints lowered; a comma in an array size makes a VLA. */
void f(int i)
{
  "foo"[i] = 0;
  ("foo")[1]++;
  --"a"[0];
  "foo"[0] += 1;
  *"foo" = 0;
  "foo"[0] = "foo"[1];
}
void f2(const char *p,int c,int a,int b,int *q){ p[c?a:b]=0; p[c?(a):(b)]=0;}
/* PR c/45079 */
/* { dg-do compile } */

void
foo3 (const char *p, int cond, int a, int b)
{
  p[cond ? a : b] = '\0';	/* { dg-error "assignment of read-only location" } */
}

/* { dg-bogus "not supported by" "" { target *-*-* } 0 } */
void f4(void){ int a[6][(2,2)]; int (*p)[3]; p = a; int b[(2,2)]; (void)sizeof(b);}
