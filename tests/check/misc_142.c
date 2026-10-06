typedef int qft(void);
volatile qft qvg;
int qvg(void);
const qft qcg;
int qcg(void);
int qvg(void) { return 1; }
typedef struct S6 { char a[8]; } S6;
void f6(void) {
  typedef int T6[1 + __builtin_has_attribute ((0, (S6){ }), aligned)];
  typedef int T6[1 + __builtin_has_attribute ((0, (S6){ }), aligned)];
  typedef int U6[1 + __builtin_has_attribute (*(S6*)0, aligned)];
  typedef int U6[1 + __builtin_has_attribute (*(S6*)0, aligned)];
  typedef int V6[1 + __alignof__ ((0, (S6){ }))];
  typedef int V6[1 + __alignof__ ((0, (S6){ }))];
  typedef int W6[1 + sizeof ((0, (S6){ }))];
  typedef int W6[1 + sizeof ((0, (S6){ }))];
}
typedef struct S4 { char a[64]; } S4;
typedef S4 __attribute__((aligned(64))) I4;
typedef I4 __attribute__((aligned(32))) A32_I4[3];
#define SA(n, e) _Static_assert(e, n)
_Static_assert(!__builtin_has_attribute((8, (A32_I4){ }), aligned), "a");
_Static_assert(!__builtin_has_attribute(((I4){ },(A32_I4){ }), aligned(32)), "b");
_Static_assert(__alignof__(((I4){ },(A32_I4){ })) == 8, "c");
_Static_assert(__alignof__((8, (A32_I4){ })) == 8, "d");
_Static_assert(!__builtin_has_attribute((0, (A32_I4){ }), aligned(64)), "e");
