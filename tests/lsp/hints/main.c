#include <fakelib.h>
struct ops { int (*cb)(int depth); int count; };
typedef int (*cbt)(int width, int height);
int two(int count, int flag);
int under(int __count, int _flag);
int trail(int count_, int flag);
int one(int value);
int setLimit(int limit);
int set_limit(int limit);
int set_Count(int COUNT);
int setup(int mode);
int set_(int x);
int unnamed(int, int);
int vari(const char *fmt, ...);
int knr();
int knrdef(a, b) int a, b; { return a + b; }
int pdef(int p, int q);
int pdef(int a, int b) { return a + b; }
#define TWO(x) two(x, 1)
#define CNT count
#define APPLY(f) f(1, 2)
void user(int count, int flag, struct ops *o, cbt fp)
{
    int Count = 3, FLAG = 4;
    two(count, flag);
    two(count + 1, (flag));
    two(Count, FLAG);
    under(count, flag);
    under(Count, _flag);
    trail(count, flag);
    two(/*count=*/1, /* flag */ 2);
    two(o->count, 2);
    one(5);
    setLimit(4);
    set_limit(1);
    set_Count(2);
    setup(2);
    set_(1);
    unnamed(1, 2);
    vari("%d %d", 9, 10);
    knr(11);
    knrdef(12, 13);
    pdef(1, 2);
    fp(15, 16);
    o->cb(17);
    (two)(1, 2);
    (*fp)(3, 4);
    TWO(20);
    two(CNT, 1);
    APPLY(two);
    __builtin_abs(1);
    implicit_fn(9);
    two(
        two(1, 2), 3);
    fake_copy(o, o, 3);
    fake_len("abc");
    fake_cmp("a", "b");
    fake_cb(0, 0, 0);
}
