#define vs(n) __attribute__((vector_size (n)))
int vs (-1) a;
int vs (0) b;
int vs (1) c;
int vs (sizeof (int)) e;
vs (1LLU << 63) char v63;
const char *p = __func__;
void f(int *i) { __atomic_exchange_n (i, 1); }
