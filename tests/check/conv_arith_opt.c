// flags: -Wconversion -Warith-conversion
short s, t; int a; unsigned char uc; char c;
void h(void) {
  s = s + 1;
  s = s + t;
  s = a + a;
  s = s + a;
  s = -s;
  s = ~s;
  uc = uc + 1;
  uc = uc * uc;
  uc = c + uc;
  s = s << 1;
  s = a / 2;
  s = (s > 0) ? s : 0;
  s = a > 0;
  s = s * 3;
  s = s % a;
}
