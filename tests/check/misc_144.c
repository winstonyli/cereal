// flags: -std=gnu99 -Wconversion -Warith-conversion -Wxor-used-as-pow
[[foo]] int f(int i){return i;}
[[__foo__]] int g(int i){return i;}
[[____noreturn____]] int h(int i){return i;}
long x1 = 2^64, x2 = 2^65, x3 = 10^30, x4 = 2^129;
void cv(char c, char d, int i, short s)
{
  c = c >> 1;
  c >>= 1;
  c = c / 2;
  c /= 2;
  c = c % 2;
  c %= 2;
  c = c >> d;
  c = c >> i;
  c >>= i;
  c = c << i;
  c <<= i;
  c = c << 1;
  c = c << d;
  c = i >> 1;
  c = i >> d;
  c = i << 1;
  c = c + 1;
  c += 1;
  c = c * i;
  c = c - d;
  c = c & i;
  c &= i;
  c = c | i;
  c = c ^ 1;
  c = s >> 1;
  c = s >> 9;
  c = c >> 9;
  c = c / d;
  c = c % i;
  c = c / i;
  c = s / 2;
}
void bw(unsigned char x, int f)
{
  x = x | (f ? 1 : 0);
  x |= f ? 1 : 0;
  x &= f ? 1 : 0;
  x ^= f ? 1 : 0;
  x = x | (f > 1);
  x |= f > 1;
  x |= 1;
  x |= 256;
  x &= f;
  x |= f;
}
