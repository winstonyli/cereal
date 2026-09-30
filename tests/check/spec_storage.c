/* storage class conflicts */
static extern int a;
typedef static int b;
register auto int c;
extern int d, static e;
int static f;
static int g;
typedef int T;
T typedef h;
__thread int i;
__thread static int j;
static __thread int k;
extern __thread int l;
auto int m;
register int n;
