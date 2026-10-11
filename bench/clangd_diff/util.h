int helper(int v) { return v; }
int hdr_fn(int v) { return helper(v); }
#define HM(x) hdr_fn(x)
#define HM2(x) HM(x)
