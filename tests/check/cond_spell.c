void f(const double *q, double *r, int c, long l, double d, int a, int b, char ch, unsigned u)
{
    *(c ? r : q) = 1;
    *(l ? r : q) = 1;
    *(d ? r : q) = 1;
    *(r ? r : q) = 1;
    *(a < b ? r : q) = 1;
    *(!c ? r : q) = 1;
    *(a && b ? r : q) = 1;
    *((c) ? r : q) = 1;
    *(a + b ? r : q) = 1;
    *(ch ? r : q) = 1;
    *(u ? r : q) = 1;
    *(c++ ? r : q) = 1;
}
