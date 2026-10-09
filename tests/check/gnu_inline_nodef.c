// flags: -std=gnu11
inline int f1 (void);
inline int __attribute__ ((gnu_inline)) f2 (void);
extern inline int __attribute__ ((gnu_inline)) f3 (void);
extern inline int f3b (void) __attribute__ ((gnu_inline));
inline int f4 (void) __attribute__ ((gnu_inline));
inline int f5 (void) __attribute__ ((always_inline));
