// flags: -Wall -Wextra
struct P { int x, y; };
int v1[] = { { 1, 2 }, { (struct P){1,2}, 3 } };
int v2[] = { { (struct P){1,2}, 3 } };
int v3 = { { 1 }, { (struct P){1,2} } };
int v4 = { (struct P){1,2} };
int v5 = { 1, (struct P){1,2} };
int v6 = { { (struct P){1,2} } };
