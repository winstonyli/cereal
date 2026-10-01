void f(long arg) {
  register long var asm ("r1");
  asm ("blah" : "r" (arg));
}
