// flags: -fdiagnostics-parseable-fixits
#define TWICE(x) ((x) + (x))
#define RET(x) return x
#define BODY return amount
int amountt;
int f(void)
{
    return TWICE(amountx);
}
int g(void)
{
    RET(amountx);
}
int h(void)
{
    BODY;
}
int k(int a, int b)
{
    return a
        + (b
}
