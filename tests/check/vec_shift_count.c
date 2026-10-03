typedef int v4 __attribute__((vector_size(16)));
v4 f(v4 x) { return x << 40; }
v4 g(v4 x) { return x >> -1; }
v4 h(v4 x) { return x << 31; }
