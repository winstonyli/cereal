int leaf(int a);
#define CL(v) leaf(v)
int pad = 22;
int two(void) { leaf(0); return CL(2); }
int three(void) { return leaf(3); }
