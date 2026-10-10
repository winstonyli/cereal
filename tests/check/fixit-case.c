// flags: -fdiagnostics-parseable-fixits
int f(int x)
{
    switch (x) {
    case 42
        return 1;
    }
    return 0;
}
