// flags: -Wsign-compare
int a, b; unsigned u; short s; unsigned short us; unsigned char uc; long l; unsigned long ul; char c;
void h(void) {
  u = a ? a : u; u = a ? u : a; u = a ? 5 : u; u = a ? -5 : u; u = a ? s : u; u = a ? us : a; u = a ? c : uc;
  ul = a ? l : ul; ul = a ? u : l; l = a ? u : l; u = a ? a & 3 : u; u = a ? (unsigned char)c : a;
  u = a ? a : 1u; u = a ? -1 : 1u; u = a ? 1 : 1u; u = a ? a >> 1 : u; u = a ? a % 5 : u;
  l = a ? a : u; a = a ? a : u; u = a ? uc : a; u = a ? s : us;
}
