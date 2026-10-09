#define A(...) __VA_OPT__ /* expect: error */
#define B(...) __VA_OPT__(1 /* expect: error */
#define C(...) __VA_OPT__(__VA_OPT__(1)) /* expect: error */
#define D(...) __VA_OPT__(## 1) /* expect: error */
#define E(...) __VA_OPT__(1 ##) /* expect: error */
#define F(x) __VA_OPT__(x) /* expect: error */
#define G(...) # __VA_OPT__ x /* expect: error */
int k = __VA_OPT__(1); /* expect: error */
A(1) B(1) F(1)
