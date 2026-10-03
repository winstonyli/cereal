// flags: -std=gnu99
void h (void) {
  int v;
  asm ("%[zz]"
       : "=r" (v)
       :
       : "memory");
  v = 1;
}
void g (void) {
  asm ("nop"
  "%[qq]" ::);
}
