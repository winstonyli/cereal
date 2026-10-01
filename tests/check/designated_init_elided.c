// flags: -std=gnu99
struct D { int x; int y; int z; } __attribute__((designated_init));
struct D a1[3] = { 1, 2, 3, 4, 5, 6, 7 };
struct D a2[2] = { {1, 2, 3}, 4, 5, 6 };
struct W { int p; struct D d; int q; };
struct W w = { 1, 2, 3, 4, 5 };
struct W w3 = { 1, { 2, 3, 4 }, 5 };
