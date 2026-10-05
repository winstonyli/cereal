// flags: -std=gnu99
/* Recovery: a braced-group outside a function is reported at its '(' and
 * skipped whole; typeof after a type specifier ends the specifiers; an
 * attribute after the declarator of a function definition is diagnosed at the
 * start of the declaration and the body is skipped. */
int a[({ int b })];
int c[({ int d () {}; })];
int typeof;
long __typeof__(1) x;
void foo (void) __attribute__((__visibility__("default")))
{
}
int ok;
