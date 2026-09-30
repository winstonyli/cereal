// flags: -Wall -Wextra
struct B { unsigned x:3; int y:4; unsigned long z:40; long w:40; unsigned u:32; int s:31; unsigned x2:20; short h:9; unsigned char uc:5;};
int f(struct B b, int i, unsigned ui, long l, unsigned long ul){ int r=0;
r += b.x < i;
r += b.x < ui;
r += b.y < i;
r += b.y < ui;
r += b.u < i;
r += b.s < ui;
r += b.z < l;
r += b.w < ul;
r += b.x2 < i;
r += b.h < ui;
r += b.uc < i;
r += i < b.x;
r += ui < b.y;
r += b.y < 1u;
r += b.y == 1u;
r += b.y < ul;
r += b.x < ul;
r += b.h == 1u;
return r;}
