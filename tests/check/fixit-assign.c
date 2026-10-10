// flags: -Wall -fdiagnostics-parseable-fixits
int f(int a, int b)
{
    if (a = b)
        return 1;
    while (a = b)
        b--;
    return a ? a = 1 : 0;
}
