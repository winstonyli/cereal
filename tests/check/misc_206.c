// flags: -O2
typedef __SIZE_TYPE__ size_t;
size_t strlen(const char *);
const char arr[7] = "abc";
const char pad[8] = "ab\0cd";
char nonconst[4] = "abc";
int z1[__builtin_strlen("abc")];
int z2[sizeof(int[__builtin_strlen("a\0bc") + 1])];
int z3[__builtin_strlen(arr)];
int z4[__builtin_strlen(arr + 1)];
int z5[strlen("hello")];
int z6[__builtin_strlen(pad)];
const char *p1 = arr + __builtin_strlen(arr) + 1;
int z7[__builtin_strlen(nonconst)];
