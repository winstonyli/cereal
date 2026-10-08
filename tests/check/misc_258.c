#define EARLY_NAME 1
int a = EARLY_NAM;
#undef EARLY_NAME
int b = EARLY_NAM;
int c = LATE_NAM;
#define LATE_NAME 2
int d = LATE_NAM;
#define MY_MACRO 1
#define _RESERVED_MACRO 2
int e = MY_MACR;
int f(void) { return MY_MACRO_; }
