// flags: -std=gnu99 -pedantic
/* __auto_type misuse (errors at the start of the declaration, the rest of it
 * skipped); a qualifier on a tag of the wrong kind; _Imaginary is a keyword
 * that cannot be a specifier; a failed declarator is not an empty
 * declaration. */
__auto_type;
__auto_type *p = (int *) 0;
struct s0 { int i : 1; } x;
void f (void) { __auto_type v = x.i; }
__auto_type i;
__auto_type g { }
__auto_type a = 1, b = 2;
int ok1 = 1;

union u3 { float v; };
struct s4 { int a; };
void h(void) { const struct u3; }
void h2(void) { const struct s4; }
const struct u3;

float _Imaginary;
void k(void) { double _Imaginary z; double ) w; int * 3; }
