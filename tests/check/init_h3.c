// flags: -Wall -Wextra
struct P { int x, y; };
int v1[] = { .s = { { 1 }, { (struct P){1,2}, 2 } } };
int v2[] = { .s = { (struct P){1,2} } };
int v3[] = { .s = (struct P){1,2} };
int v4[] = { .s = 1, (struct P){1,2} };
int v5[] = { .s = { 1 }, (struct P){1,2} };
