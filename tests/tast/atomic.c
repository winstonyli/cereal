/* T3: the generic __atomic builtins are intrinsics with their arguments as they are. */
int ld(int *p) { int r; __atomic_load(p, &r, 5); return r; }
void st(long *p, long *v) { __atomic_store(p, v, 5); }
int xc(int *p, int *v) { int r; __atomic_exchange(p, v, &r, 5); return r; }
int cx(int *p, int *e, int *d) { return __atomic_compare_exchange(p, e, d, 0, 5, 0); }
