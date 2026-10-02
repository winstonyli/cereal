// flags: -Woverflow -Wconversion
unsigned char h1(unsigned char p) { return p & 0x302; }
unsigned char h2(unsigned char p) { return p & 0x300; }
unsigned char h3(signed char p) { return p & 512; }
unsigned char h4(unsigned short p) { return p & 0x10000; }
unsigned char h5(unsigned char p) { return p & 0x1ff; }
signed char h8(unsigned char p) { return p & 0x80; }
unsigned char h9(unsigned short p) { return p & 0x100; }
unsigned char h10(unsigned short p) { return p & 0x1f0; }
unsigned char h11(unsigned char p) { return 512 & p; }
unsigned char k1(unsigned char p) { return p & 512; }
unsigned char k2(unsigned char p) { return p & 255; }
unsigned char k3(unsigned char p, int q) { return p & q; }
unsigned char k4(unsigned char p) { return p | 512; }
unsigned char k5(unsigned char p) { return p & 0x1ff; }
unsigned char k6(unsigned char p) { return p ^ 256; }
int b;
void q(void) {
  unsigned char c;
  c = b ? 256 : 512;
  c = b ? 256 : 1;
}
