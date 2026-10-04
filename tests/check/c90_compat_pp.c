// flags: -std=c99 -Wc90-c99-compat
int fn();
#define F(a) fn(a)
#define G(a, b) fn(a##b)
#define V1(...) fn(__VA_ARGS__)
#define V2(x, ...) fn(x __VA_ARGS__)
#define H() 1
void t(void) {
int a = F();
int b = F( );
int c = G(,1);
int d = G(1,);
int e = G(,);
int f = H();
int g = V1();
int h = V1(1,2);
int ii = V2(1);
int j = V2(1,);
int z = F(
);
}
