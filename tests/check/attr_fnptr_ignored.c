/* function-only attributes on a pointer-to-function variable are ignored (decl attributes) */
int (*v1)(int) __attribute__((pure));
int (*v2)(int) __attribute__((cold));
int (*v3)(int) __attribute__((hot));
int (*v4)(int) __attribute__((noinline));
int (*v5)(int) __attribute__((always_inline));
int (*v6)(int) __attribute__((returns_twice));
int (*v7)(int) __attribute__((nothrow));
int (*v8)(int) __attribute__((const));        /* applies to the pointed-to function type */
int (*v9)(int) __attribute__((noreturn));     /* so does this */
int (*v10)(int) __attribute__((pure, cold));
int (*v11)(int) __attribute__((const, pure));
typedef int (*T1)(int) __attribute__((pure));
struct S { int (*f)(int) __attribute__((hot)); };
void g(int (*p)(int) __attribute__((cold)));
static int (*v12)(int) __attribute__((nothrow));
void h(void) { int (*l)(int) __attribute__((pure)); (void)l; }
