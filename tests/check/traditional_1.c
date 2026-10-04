// flags: -Wtraditional
union U { int i; long l; };
struct S { int a; union U u; };
static void sf(void);
void sf() {}
extern int ef(void);
static int ef2(void);
int ef2(void) { return 0; }
void f(long l, int i, char c, unsigned char uc)
{
  i = +1;
  i = +i;
  i = + i + +i;
  c = '\a';
  c = '\x2';
  c = '\e';
  c = '\n';
  const char *p = "a" "b";
  p = "a" "b" "c";
  p = ("a"
       "b");
  switch (l) { default: break; }
  switch (c) { default: break; }
  switch (uc) { default: break; }
  switch ((short)i) { default: break; }
  switch (i + 0L) { default: break; }
  { struct S s = { 0, { 0 } }; }
  { struct S s = { 0, { 1 } }; }
  { union U u = { 1 }; }
  { union U u = { 0 }; }
  { union U u = { .l = 0 }; }
  { union U u = { .l = 1 }; }
  { static union U u = { 1 }; }
  { union U u; u = (union U){ 1 }; }
  { struct S s = { 1, 2, 3 }; }
  { struct S s; struct S t = s; }
  { int a[2] = { 1, 2 }; }
  { char s[] = "abc"; }
  { struct S s = { .a = 1 }; }
  { union U u[2] = { { 1 }, { 2 } }; }
}
