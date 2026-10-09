/* tags and members: forward declaration, shadowing `struct S;`, anonymous
 * members, designators (nested and through anonymous members), offsetof,
 * enums inside structs */
#define offsetof(T, m) __builtin_offsetof(T, m) /* no system header: golden stays host-independent */
typedef unsigned long size_t;
struct S;
struct S { int a; struct { int in; }; union U { int u1; float u2; } u; };
void f(void)
{
    struct S;
    struct S { int other; } s2 = { .other = 1 };
    (void)s2;
}
struct S s = { .a = 1, .in = 2, .u.u1 = 3 };
size_t off = offsetof(struct S, u.u2);
struct E { enum K { K1, K2 } k; } e = { K2 };
int g(struct S *p) { return p->in + p->u.u1 + (int)sizeof(union U); }
