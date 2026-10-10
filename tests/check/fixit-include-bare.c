// flags: -fdiagnostics-parseable-fixits
int f(const char *s)
{
    puts(s);
    return (int)strlen(s);
}
