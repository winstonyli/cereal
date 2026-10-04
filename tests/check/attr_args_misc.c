// flags: -std=gnu99 -Wall
void* my_calloc(unsigned, unsigned) __attribute__((alloc_size(1,bar)));
void* my_realloc(void*, unsigned) __attribute__((alloc_size(bar)));
void f1(char*) __attribute__((nonnull(bar)));
void f2(char*) __attribute__((nonnull(1,bar)));
void foo(int);
void f3(char*) __attribute__((nonnull(foo)));
void f4(char*) __attribute__((nonnull(1,foo)));
typedef char vec __attribute__((vector_size(foo)));
__attribute__((patchable_function_entry (-1))) void p1 (void) {}
__attribute__((patchable_function_entry (5, -5))) void p2 (void) {}
int i, j;
__attribute__((patchable_function_entry (i))) void p3 (void) {}
void
 __attribute__((patchable_function_entry(65536,1)))
p4 (void) {
}
void fnone (void);
void* __attribute__((alloc_size (2))) fa2 (int, int);
void* __attribute__((alloc_size (2, 4))) fa24 (int, int, int, int);
void* __attribute__((alloc_align (2))) fl2 (int, int);
#define A(e, x, a) _Static_assert (e == __builtin_has_attribute (x, a), #a)
void t (void)
{
  (void)__builtin_has_attribute (fnone, alloc_align (1));
  (void)__builtin_has_attribute (fnone, alloc_size (1));
  (void)__builtin_has_attribute (fa2, alloc_size (1));
  (void)__builtin_has_attribute (fa2, alloc_size (2));
  (void)__builtin_has_attribute (fa24, alloc_size (2));
  (void)__builtin_has_attribute (fa24, alloc_size (2, 4));
  (void)__builtin_has_attribute (fl2, alloc_align (1));
  (void)__builtin_has_attribute (fl2, alloc_align (2));
}
