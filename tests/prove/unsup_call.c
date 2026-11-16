/* A call needs contracts at call sites: unsupported for now. */
int f(int);
int g(int x)
{
#pragma cereal ensures return == 0
    return f(x) - f(x);
}
