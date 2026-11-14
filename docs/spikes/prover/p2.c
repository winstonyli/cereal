void fill2(char *d, unsigned long r, unsigned long c){ unsigned long i, j; for (i = 0; i < r; i++) for (j = 0; j < c; j++) d[i * c + j] = 0; }
