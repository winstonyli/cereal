#define F 1 + ## -
#define G(x) 1 x ## - 2
#define W(x) G(x)
int a = F 2;
int b = G(+);
int c = W(+);
