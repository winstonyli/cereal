/* Syntax errors are reported once and recovered from locally. */
int a = ;                       /* missing expression */
int b;                          /* parses */

void missing_semi(void) {
    int x = 1
    int y = 2;                  /* one error, then back in sync */
    x = y;
}

void unclosed(void) {
    if (a) {
        b = 1;
    /* the '}' of the if is missing: the function ends at the next
     * definition in column 0 */
}

int after(void) { return b; }   /* a function definition of its own */

struct s { int m int n; };      /* member recovery */
int c = (1 + ;                  /* expression recovery */
int d;
