/* File-scope compound literals are anonymous static objects (gcc: __compound_literal.N). */
struct N { int u; };
struct T { long a; struct N n; int v[2]; };
struct P { int *p; struct T *t; };
int *sp = (int[]){ 1, 2 };
struct T *tp = &(struct T){ .a = 7, .n = { 3 }, .v = { 8, 9 } };
struct P pp = { (int[]){ 5, 6 }, &(struct T){ .a = 1 } };
char *cs = (char[]){ "hi" };
int *off = (int[]){ 1, 2, 3 } + 1;
int *in = &(int[]){ 4, 5 }[1];
