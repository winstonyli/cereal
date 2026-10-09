#ifndef SHAPES_H
#define SHAPES_H
struct rect { int w, h; unsigned flags : 3, kind : 5; };
union cell { int i; float f; char c[8]; };
enum mode { MODE_SLOW = -1, MODE_FAST = 4 };
struct big {
    int f1, f2, f3, f4, f5, f6, f7, f8, f9, f10;
    int f11, f12, f13, f14, f15, f16, f17, f18, f19, f20;
};
#endif
