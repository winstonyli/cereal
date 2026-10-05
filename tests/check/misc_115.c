// flags: -std=gnu99
/* The pedantic warning for an omitted middle operand comes before the
 * warnings of the third operand, as gcc's parser reports it first. */
int f(int a) { return a ? : ({ 1; }); }
int g(int a) { return ({ 2; }) ? : 3; }
int h(int a, int b) { return a == b ? : ({ a; }); }
