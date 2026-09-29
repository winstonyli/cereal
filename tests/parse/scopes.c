/* Typedef-name scoping (after Jourdan & Pottier's test cases).  Each
 * function's comment says how the marked statements must parse. */
typedef int T;

/* a block-scope declaration hides T until the block ends */
void block(void) {
    { int T; T = 1; }         /* T: variable -> expression */
    T y;                      /* T: type again -> declaration */
}

/* a declarator is in scope before its initializer */
void init(void) {
    int T = sizeof(T);        /* sizeof applies to the variable */
}

/* `T T;` declares a variable T; after it T is not a type */
void same(void) {
    T T;
    T = 2;                    /* expression */
}

/* parameters are in scope in the body, and only there */
void param(int T) {
    T = 3;                    /* expression */
}
T after_param;                /* declaration */

/* later parameters see earlier ones */
void later(int T, int n[sizeof(T)]);

/* in a parameter declaration, (T) is a function declarator */
void abstract(int (T));

/* T(x) declares x where T is a type, calls T where it is not */
void call_or_decl(void) {
    T(x);                     /* declaration of x */
    { int T; T(x); }          /* call */
}

/* an enumeration constant hides the typedef */
void enum_const(void) {
    enum { T };
    T + 1;                    /* expression */
}

/* for-loop declarations end with the loop */
void loop(void) {
    for (int T = 0; T < 1; T++)
        T = 1;                /* expression */
    T z;                      /* declaration */
}

/* selection statements are blocks: the enum constant ends with the if */
void selection(void) {
    if (sizeof(enum { T = 1 }))
        T = 2;                /* expression: T is the constant */
    T w;                      /* declaration */
}

/* member names do not hide anything */
struct S { int T; } s;
T member_after;

/* casts, sizeof and compound literals of typedef names */
void casts(void) {
    int a = (T)1 + sizeof(T) + (T){2};
    (void)a;
}

/* a label named like a type */
void label(void) {
T:  goto T;
}

/* `unsigned T:3` names a bit-field T; `const T:3` is an unnamed one of
 * type T */
struct B { unsigned T:3; const T:3; } bits;

/* T * b is a declaration where T is a type, a product where it is not */
void star(void) {
    T * p;                    /* declaration */
    { int T, b; T * b; }      /* expression */
}

/* the parameters of f end with f's parameter list: the list of the
 * returned function type sees the typedef again */
typedef long U;
enum { V } (*fn(T T, enum { U } y, int x[T + U]))(T t);
U after_fn;                   /* U: the typedef again */

/* a nested function declarator's parameters are not the outer one's */
typedef int T2;
int nest(int g(int T2)) { T2 y; return y; }
