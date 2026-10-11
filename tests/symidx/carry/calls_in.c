int leaf(int a);
#define CL(v) leaf(v)
int one(void) { return leaf(1); }
int two(void) { leaf(9); return CL(2); }
int three(void) { return leaf(3); }
