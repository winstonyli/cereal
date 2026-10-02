// flags: -Warith-conversion -Wconversion
short s, t; unsigned short us; signed char c; unsigned char uc; int i;
void f(void){
  s = s / 2;      /* 3 */
  s = s / t;
  s = s / i;
  s = s % 3;
  s = s % t;
  s = s >> 1;
  s = s >> t;
  s = s << 1;
  s = s << t;
  s = us / 2;
  us = us / 2;
  us = us % uc;
  us = us >> 3;
  c = c / 2;
  c = uc / 2;
  c = c % 3;
  c = c >> 1;
  c = s / 2;
  c = i / 2;
  c = i >> 1;
  c = us >> 9;
  c = us >> 8;
  c = us >> 7;
  s = (s + t) / 2;
  s = c / t;
}

void g(void){
  s = s - s;
  s -= s;
  s ^= s;
  s = s ^ s;
  s = (s) - (s);
  s = s - t;
  s = i - i;
  uc = uc - uc;
  uc -= uc;
  s = l ^ l;
  s = (s + t) - (s + t);
  s = i - i + 1;
}
