// flags: -std=gnu99 -Wall -Wextra
typedef _Complex float cm __attribute__((aligned(1)));
typedef int ai __attribute__((aligned(1)));
struct b { char c; cm x[4]; ai y; int z; } __attribute__((packed));
struct b g;
void f(int k){ cm *p = &g.x[k]; ai *q = &g.y; int *r = &g.z; cm *p2 = &g.x[0]; _Static_assert(_Alignof(cm)==1,""); }
int fn1(int i){ return ({ i; }) == ({ i; }); }
int fn2(int i){ return ({ i + 1; }) != ({ i + 1; }); }
int fn3(int i){ return ({ i; }) == ({ i + 1; }); }
