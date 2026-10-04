// flags: -std=gnu99
int a b c;
int a1 b1 = 1;
int a3 b3 *c3;
int a4 b4 q4 r4;
int a5 b5 c5 d5;
void f(void) { int a6 b6 c6; int a7 b7; }
typedef __CHAR16_TYPE__ char16_t;
char16_t c16 = u'\U00064321';
int m __attribute__((mode(foo)));
int Àx __attribute__((Á));
void g(void) { asm ("%[Ã]" : : ); }
int Ä b8 c8;
