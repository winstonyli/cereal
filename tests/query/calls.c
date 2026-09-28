#define LEAF 1
#define MID(x) (LEAF + x)
#define TOP(y) MID(y) * OTHER
#define CAT(a, b) a ## b
#define MI MID
#define VIA CAT(MI, D)(2)
int a = TOP(3);
int b = VIA;
int c = MID(LEAF);
