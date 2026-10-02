// flags: -std=gnu99
// vector casts and __builtin_shufflevector checks
typedef int v2si __attribute__((vector_size(8)));
typedef float v2sf __attribute__((vector_size(8)));
typedef long long v1di __attribute__((vector_size(8)));
struct S { int a; };
enum E { A };
void *p; struct S s; _Bool bb; _Complex int ci; float f; double d; long long ll; int i; short sh; v2sf vf; v2si vi; v4si v4;
void f1(void) {
  (v4si) 1;       /* 1 int -> 16 */
  (v2si) ll;      /* same size */
  (v4si) ll;
  (v2sf) d;
  (v2sf) ll;
  (v2sf) f;
  (v2si) p;
  (v2si) s;
  (v2si) A;
  (v2si) bb;
  (v2si) ci;
  (v1di) vi;
  (v4si) vi;
  (int) vi;
  (long long) vi;
  (double) vi;
  (void *) vi;
  (_Bool) vi;
  (struct S) vi;
  (v2si) 0;
  (v2si) sh;
}
