int a __attribute__((aligned(3)));
int b __attribute__((aligned(-4)));
int c __attribute__((aligned(1 << 29)));
int d __attribute__((aligned(16)));
_Alignas(1 << 29) int e;
_Alignas(5) int f;
