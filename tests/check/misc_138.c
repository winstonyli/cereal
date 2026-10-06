typedef int IA[]; typedef int A10[10];
static IA *b17;
void g(void){ sizeof(*b17); { extern A10 *b17; sizeof(*b17); } sizeof(*b17); }
static IA *c17(void);
void h(void){ sizeof(*c17()); { extern A10 *c17(void); sizeof(*c17()); } sizeof(*c17()); }
static IA *c1;
void k1(void){ { extern A10 *c1; } sizeof(*c1); }
IA *c2;
void k2(void){ { extern A10 *c2; } sizeof(*c2); }
static IA *c3;
void k3(void){ { extern IA *c3; } sizeof(*c3); }
void rf(void){
  register int a[2];
  int *p = a;
  a;
  (void)a;
  a + 1;
  a[0];
  *a;
  sizeof a;
}
struct S { volatile int field; };

void
rg (void)
{
  register struct S a;
  register struct S b[2];
  register struct S c __asm__("nosuchreg"); /* { dg-error "object with volatile field" "explicit reg name" } */
  &a; /* { dg-error "address of register" "explicit address" } */
  b; /* { dg-error "address of register" "implicit address" } */
}
