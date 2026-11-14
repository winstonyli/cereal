unsigned long blen(const char *s, unsigned long n){ unsigned long i = 0; while (i < n && s[i] != 0) i++; return i; }
