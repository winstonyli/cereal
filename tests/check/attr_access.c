#define RW(...) __attribute__ ((access (read_write, __VA_ARGS__)))
#define RO(...) __attribute__ ((access (read_only, __VA_ARGS__)))
typedef int F (int, int);

int __attribute__ ((access))
none (void);
int __attribute__ ((access (rdonly)))
bad_mode (void);
int __attribute__ ((access (read_only)))
no_arg (void);
int __attribute__ ((access (read_only ())))
call_mode (void);
int RO (1, 1) f1 (const void *, const int *);
int RO (4) f2 (int, int, int);
int RO (1) f3 (int);
int RO (-1) f4 (const void *);
int RO (1, "s") f5 (const void *, int);
int RW (1) f6 (const char *);
int RO (1) f7 (F *);
int RO (1, 2) f8 (const void *, int);
void RO (1, 2) (*pf) (int, int);
