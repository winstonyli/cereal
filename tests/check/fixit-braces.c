// flags: -Wall -fdiagnostics-parseable-fixits
struct P { int a[2]; int b; };
int a1[2][2] = {1, 2, 3, 4};
struct P p1 = {1, 2, 3};
struct P p2[2] = {1, 2, 3, 4, 5, 6};
int a3[2][2][2] = {1, 2, 3, 4, 5, 6, 7, 8};
int a4[2][2] = {1 , 2 , 3 , 4 ,};
int a5[2][2] = {{1, 2}, 3, 4};
void use(struct P);
void g(void) { use((struct P){1, 2, 3}); }
