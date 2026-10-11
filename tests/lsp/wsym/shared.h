#ifndef SHARED_H
#define SHARED_H
#include <sysdef.h>
#define SHARED_MAX 10
#define SHARED_CALL(v) leaf(v, 0)
struct point { int x; int y; };
enum shade { SHADE_DARK, SHADE_LIGHT };
typedef struct point point_t;
int leaf(int a, int b);
extern int shared_counter;
struct TString { int len; };
int luaC_fix(void);
#endif
