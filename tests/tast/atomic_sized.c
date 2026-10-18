/* T6: the sized __atomic_ and __sync builtins are intrinsics too. */
int ld(int *p) { return __atomic_load_n(p, 5); }
void st(long *p, long v) { __atomic_store_n(p, v, 5); }
int fa(int *p) { return __atomic_fetch_add(p, 1, 5); }
long sf(long *p) { return __sync_fetch_and_sub(p, 2); }
int cas(int *p, int o, int n) { return __sync_bool_compare_and_swap(p, o, n); }
int cx(int *p, int *o, int n) { return __atomic_compare_exchange_n(p, o, n, 0, 5, 5); }
int gen(int *p, int *o) { __atomic_load(p, o, 5); return *o; }
