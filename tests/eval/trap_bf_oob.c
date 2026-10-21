// trap: out of bounds
struct S { unsigned a : 4, b : 4; };

int main(void)
{
    struct S s = {1, 2};
    struct S *p = &s + 1;
    return p->b;
}
