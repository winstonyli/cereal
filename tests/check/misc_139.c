void af1(void);
__attribute__((aligned(64))) void af2(void);
void af3(void) __attribute__((aligned(32)));
int xa = __alignof__(af1), b = __alignof__(af2), c = __alignof__(af3), d = __alignof__(void(void));
_Static_assert(__alignof__(af1)==1,"1");
_Static_assert(__alignof__(af2)==64,"2");
_Static_assert(__alignof__(af3)==32,"3");
__attribute__((aligned(0x10000000))) static const char asc = 0;
_Static_assert(__alignof__(asc) == 0x10000000, "a");
__attribute__((aligned(0x10000000))) char av;
_Static_assert(__alignof__(av) == 0x10000000, "b");
#define TEN "          "
#define HUN TEN TEN TEN TEN TEN  TEN TEN TEN TEN TEN
#define THO HUN HUN HUN HUN HUN  HUN HUN HUN HUN HUN
#define BIG THO THO THO THO TEN TEN TEN TEN TEN TEN TEN TEN TEN "123456"
__asm__ (BIG);
int ovv __asm__(BIG);
void ovf(int x) { __asm__ (BIG); __asm__ goto (BIG : : : : l); l:; _Static_assert(1, BIG); const char *ovs = BIG; int ovw __attribute__((section(BIG))); }
