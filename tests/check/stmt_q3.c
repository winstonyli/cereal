// flags: -Wall -Wextra
void g(int);
void f(int a, int b)
{
    if(a) if(b) g(1); else g(2);
    if (a)
        if (b)
            g(1);
        else
            g(2);
    if (a) while (b) if (b) g(1); else g(2);
    if (a) { if (b) g(1); else g(2); }
    if (a) if (b) g(1); else g(2); else g(3);
    if (a) for (;;) if (b) g(1); else g(2);
    if (a) g(1); else if (b) g(2); else g(3);
    if (a) if (b) g(1);
    else g(3);
}
