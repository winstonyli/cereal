typedef __attribute__((aligned (2))) char CA2;
int faligned_8 __attribute__((aligned (8)));
void f(void) {
  int b, i = 1;
  b = __builtin_has_attribute (CA2[2], aligned);
  b = __builtin_has_attribute (faligned_8, aligned (i));
  b =
    __builtin_has_attribute (faligned_8, aligned (i));
  b = 1 +
    __builtin_has_attribute (CA2[2], aligned);
}
