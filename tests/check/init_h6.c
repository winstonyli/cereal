// flags: -Wall -Wextra
struct P { int x, y; };
struct Q { struct P p; int z; };
struct Q v0[] = { 0 };
struct Q v1[] = { 1, 2, 3, 4 };
struct P v2[] = { 1, 2, 3 };
int v3[][2] = { 1, 2, 3 };
int v4[][2] = { 1 };
struct Q v5[] = { .p.y = 1 };
