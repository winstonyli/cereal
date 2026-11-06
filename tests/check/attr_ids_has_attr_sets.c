// flags: -Wall
/* e_has_attr gathers up to four attribute sets for a type name: two typedefs, the record and the
 * attributes written in the type name.  With all four in play the last one used to write past a
 * 3-element array (found by asan; the san gate stage runs this file). */
struct __attribute__((packed)) P { char c; int i; };
typedef struct P T1 __attribute__((u1));
typedef T1 T2 __attribute__((u2));
#define YES(n, e, a) extern char n[__builtin_has_attribute (e, a) ? 1 : -1]
YES(a1, T2 __attribute__((deprecated)), deprecated);
YES(a2, T2 __attribute__((deprecated)), packed);
