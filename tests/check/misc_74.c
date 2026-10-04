enum E { A __attribute__((deprecated)), B, U __attribute__((unavailable("gone"))) };
int f(int i){ i += A; i += B; i += U; return i; }
enum F { C __attribute__((deprecated("use D"))) = 3 };
int g(void){ return C; }
