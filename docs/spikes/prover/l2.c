void rev(char *d, const char *s, unsigned long n){ unsigned long i, j = n; for (i = 0; i < n; i++) { j--; d[i] = s[j]; } }
