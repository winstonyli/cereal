void f(void){
  int i = 1;
  typedef int A[1];
  typedef int A[1 - 2 * !(__builtin_has_attribute(f, aligned(i)) == 0)];
}
