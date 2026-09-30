typedef unsigned long size_t;
typedef struct node { struct node *next; int v; } node_t;
enum kind { K_A, K_B = 5, K_C };
union u { int i; float f; char c[4]; };
struct bits { unsigned a : 3, b : 5; int : 0; unsigned c : 1; };
extern int ext[];
static const int tbl[] = { 1, 2, 3 };
int (*fp)(int, char *);
int (*arr_of_fp[4])(void);
void (*signal(int, void (*)(int)))(int);
const char *const names[] = { "a", "b" };
int f(int n, int a[n]) { int v[n]; return sizeof v / sizeof v[0] + a[0]; }
static inline int sq(int x) { return x * x; }
_Static_assert(sizeof(size_t) == 8, "lp64");
struct fl { int n; char d[]; };
node_t *mk(void);
int g(void);
int g(void) { return 0; }
long long ll; unsigned long long ull; long double ld; _Bool flag;
double _Complex z;
int main(void) { return g(); }
