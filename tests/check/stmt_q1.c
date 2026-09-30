// flags: -Wall -Wextra
void f(int i, unsigned u, long l, unsigned long ul, char c, unsigned char uc, short s, long long ll)
{
    switch (i) { case 4294967296L: break; case 3000000000U: break; case -1L: break; case 2147483648: break; }
    switch (u) { case -1: break; case 5000000000: break; case -5000000000: break; case 4294967296UL: break; }
    switch (ul) { case -1: break; case -1L: break; case 1: break; }
    switch (l) { case 18446744073709551615UL: break; case 1: break;}
    switch (c) { case 300: break; case -300: break; case 'a': break; case 128: break; }
    switch (uc) { case 256: break; case -1: break; case 255: break; }
    switch (s) { case 40000: break; case 1 ... 70000: break; case -70000 ... 5: break; }
    switch (ll) { case 1: break; }
}
