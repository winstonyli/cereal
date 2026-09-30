// flags: -Wall -Wextra
struct P { int a, b; };
struct Q { struct P p; int c[2]; };
struct Q q1 = { 1, 2, 3, 4 };
struct Q q2 = { { 1, 2 }, 3, 4 };
int a2[2][2] = { 1, 2, 3, 4 };
int a3[2][2] = { { 1 }, 2, 3 };
struct P pa[2] = { 1, 2, 3, 4 };
int o1[3] = { [0] = 1, [0] = 2, [1] = 3 };
struct P o2 = { .a = 1, .a = 2, .b = 3 };
struct P o3 = { 1, .a = 2 };
char s1[2][3] = { "ab", "cd" };
char s2[2][3] = { "abc", "def" };
char s3[3] = "abcd";
struct Q q3 = { .c = { 1 }, .p.b = 2 };
union U { int i; float f; } u1 = { 1, 2 };
int o4[5] = { [1 ... 3] = 1, [2] = 5 };
