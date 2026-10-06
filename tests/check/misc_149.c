// flags: -Wall
extern __attribute__((nonstring)) void f1(void);
void f2(void) __attribute__((nonstring));
struct __attribute__((nonstring)) S { int i; };
typedef char __attribute__((nonstring)) nschar_t;
char __attribute__((nonstring)) ok[4];
int __attribute__((nonstring)) bad;
