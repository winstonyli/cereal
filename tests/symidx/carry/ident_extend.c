int foobar(int a) { return a; }
int use(void)
{
    int foo2 = 1;
    return foo(foo2);
}
int other(void) { return foo(3); }
