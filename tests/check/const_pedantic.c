// flags: -pedantic-errors
int a = -1 << 0;
int b = 1 ? 0 : (2, 3);
enum { E1 = 2 || 1 / 0, E2 = __INT_MAX__ + 1 };
struct s { int : 0 * (__INT_MAX__ + 1); };
int f(int i) {
    switch (i) {
    case 1 + 0 * (__INT_MAX__ + 1): return 1;
    }
    return 0;
}
