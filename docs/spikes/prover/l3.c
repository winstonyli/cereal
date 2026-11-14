void fillnz(char *d, const char *s, unsigned long n){ unsigned long i; for (i = 0; i < n; i++) { if (s[i] != 0) d[i] = s[i]; else d[i] = 1; } }
