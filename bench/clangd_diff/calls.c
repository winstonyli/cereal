#include "util.h"
#define MAC(x) helper(x)
int counter;
int leaf(int v);
int pair(void) { return 0; }
int a = leaf(1), b = pair();
int mid(int v) { return leaf(v) + MAC(v); }
int top(int v) { int local = mid(v); return local + mid(2) + hdr_fn(1); }
int use_hm(void) { return HM(1); }
