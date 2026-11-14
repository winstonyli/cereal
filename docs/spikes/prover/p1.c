void pcopy(char *d, const char *s, unsigned long n){ const char *e = s + n; while (s < e) *d++ = *s++; }
