void *f3 (void) __attribute__((alloc_align));
void *f4 (int, int) __attribute__((alloc_align (1, 2)));
void *f5 (void) __attribute__((assume_aligned (32, 16, 8)));
__attribute__ ((copy)) void
f6 (void);
