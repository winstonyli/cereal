// Overflow in a call argument is reported at the argument's first token.
#define M 127
void fsc(signed char);
void fuc(unsigned char);
void h(int n)
{
  fsc(200 + 1);
  fsc(M + 1);
  fsc(1 + M);
  fsc(255);
  fuc(256);
  fsc(M + n + 1);
}
