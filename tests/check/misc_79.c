#include <limits.h>
#include <stddef.h>
int a = 1 + INT_MAX;
int b = (int)(double)1.0 + INT_MAX;
int f(int j) { return j ? (int)(double)1 + INT_MAX : 0; }
int* arr[1] = { 0, NULL };
struct S { int *a; } s = { 0, NULL };
struct __attribute__((designated_init)) S2 { int *a; } s2 = { NULL };
