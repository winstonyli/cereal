// flags: -Wconversion
void fd(double); void fi(int); void fu(unsigned); void fcf(float _Complex);
void fci(int _Complex); void fcu(unsigned _Complex);
double _Complex dc; int _Complex ic; unsigned _Complex uc;
long long unsigned _Complex llc; float _Complex fc;
void f(void)
{
    fi(dc);
    fu(ic);
    fd(uc);
    fci(dc);
    fcu(llc);
    fcf(dc);
    fci(1 + 1i);       /* constant: not modelled, stays silent */
    fi(ic = dc);
}
