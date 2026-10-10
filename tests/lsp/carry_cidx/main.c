#define LIMIT 10
typedef struct { int x; int y; } point_t;
static int total = 0;
int helper(int a, int b);

int helper(int a, int b)
{
    return a + b + total;
}

int main(void)
{
    int v = helper(1, 2);
    return v;
}

int tail_fn(point_t p)
{
    return p.x + total;
}
