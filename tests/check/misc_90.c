int (*A)[];
extern int arr[];
struct S { int n; int y[]; } s;
typedef int T[];
struct U { int n; T z; };
void f(struct S *p, struct U *u)
{
    1234, A += 1;
    A + 1;
    1234 && &s.y + 1;
    &p->y + 1;
    &u->z + 1;
    &arr + 1;
}
