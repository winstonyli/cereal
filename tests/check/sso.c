// flags: -std=c99 -pedantic
int i;
struct __attribute__((scalar_storage_order("big-endian"))) Rec { int *p; };
struct Rec r = { &i };
struct __attribute__((scalar_storage_order("little-endian"))) Lrec { int *p; };
struct Lrec l = { &i };
struct S { int i; };
typedef struct S __attribute__((scalar_storage_order("big-endian"))) S1;
typedef struct S __attribute__((scalar_storage_order("little-endian"))) S2;
typedef struct S __attribute__((scalar_storage_order("other"))) S3;
struct __attribute__((scalar_storage_order("x"))) Bad { int i; };
struct S sv;
S1 s1v;
void f(struct S *s, S1 *s1, S2 *s2)
{
  *s = *s1, *s = *s2;
  *s1 = *s2;
}
