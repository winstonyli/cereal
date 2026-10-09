/* labels (goto, &&label, __label__ in nested blocks), _Generic, macro
 * bodies, arguments and pasted names */
#define BODY_USE (counter + 1)
#define ARG(x) (x)
#define PASTE(a, b) a##b
int counter, pasted_name;
int f(int n)
{
    void *p = &&out;
    if (n)
        goto out;
    {
        __label__ inner;
        goto inner;
    inner:;
    }
    n = _Generic(n, int: counter, default: n);
    return BODY_USE + ARG(n) + PASTE(pasted, _name);
out:
    return p != 0;
}
