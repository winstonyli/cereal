/* C2X features accepted under -std=c99 with pedwarns. */
typedef struct S { int a; } S_t;
enum E : unsigned char { EA, EB };
[[deprecated]] int dep(void);
int f(void)
{
    int *p = (static int[]){1, 2};
    int x = 0b101;
L:
    int y = x;
    return *p + y;
}
union U { int i; float f; } __attribute__((transparent_union));
void tu(union U u);
void g(int i) { tu(i); }
