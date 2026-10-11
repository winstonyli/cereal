#include "shared.h"
#define A_LEAF_ATTR 1
static int helper_leaf(int v) { return v + SHARED_MAX; }
int leaf(int a, int b) { return a + b + helper_leaf(a); }
int shared_counter;
struct { int hidden; } anon_obj;
