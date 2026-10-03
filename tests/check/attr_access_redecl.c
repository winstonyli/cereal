// flags: -Wall
#define A(m, ...) __attribute__((access(m, __VA_ARGS__)))
int A(read_write, 1) f (void*, int, int);
int A(read_write, 1, 2) f (void*, int, int);
int A(read_write, 1, 3) f (void*, int, int);
int A(read_write, 1) g (void*, int, int);
int A(read_write, 1, 2) g (void*, int, int);
int A(read_write, 1) g (void*, int, int);
int A(read_write, 1, 2) h (void*, int, int);
int A(read_write, 1) h (void*, int, int);
int A(read_write, 1, 3) h (void*, int, int);
int A(none, 1) k (void*, int, int);
int A(read_only, 1) k (void*, int, int);
int A(read_write, 1) m (void*, int, int);
int A(read_write, 1) m (void*, int, int);
int A(read_only, 1) m (void*, int, int) { return 0; }
int A(__read_write__, 1) q2 (void*);
int A(read_write, 1) q2 (void*);
int A(write_only, 1) q2 (void*);
