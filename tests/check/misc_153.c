// flags: -Wc++-compat
int a = __builtin_has_attribute (struct A { int i; }, aligned);
int b = __builtin_has_attribute (int, aligned);
