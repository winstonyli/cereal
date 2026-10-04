// flags: -Wfloat-equal -Wunsuffixed-float-constants -Wenum-compare -Wswitch-enum -Waddress
enum E { A, B, C };
enum F { D, G };
int f1(double x, float y, _Complex double z)
{
    if (x == 1.0) return 1;
    if (y != 2) return 2;
    if (z == z) return 3;
    return !x;
}
double f2(void) { return 1.5 + 2.5f + 3.5L; }
int f3(void) { return A > D; }
int f4(enum E e)
{
    switch (e) {
    case A: return 0;
    case (enum E)7: return 1;
    }
    return 2;
}
int f5(void) { return "x" == (void *)0 || "y" == 0; }
