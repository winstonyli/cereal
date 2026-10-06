struct S {int a;};
void f(__builtin_va_list ap){
  __builtin_va_arg(ap, *);
  __builtin_va_arg(ap);
  __builtin_va_arg(ap, int, 1);
  __builtin_va_arg();
  __builtin_offsetof(*, x);
  __builtin_offsetof(struct S);
  __builtin_offsetof(struct S, );
  __builtin_types_compatible_p(*, int);
  __builtin_types_compatible_p(int);
  __builtin_types_compatible_p(int, *);
  __builtin_types_compatible_p(int, int, 1);
}
