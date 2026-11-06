// flags: -Wall
/* docs/ATTR_IDS_DESIGN.md golden (c): `packed` on a variable is dropped from its set but the entry
 * stays, so copy(v) still copies a (tombstone-only) set into y and that set fills a slot of the
 * 3-slot cexpr_asets.  Pins cereal's cap, not gcc's (gcc says 1); see "Found" in the design.
 * z copies a set whose only entry is unknown: copy_excluded meets an unknown entry (this only guards
 * against a crash there; the result is 0 whatever copy_excluded says). */
struct __attribute__((packed)) P { char c; int i; };
typedef struct P T1 __attribute__((u1));
typedef T1 T2 __attribute__((u2));
int v __attribute__((packed));
T2 y __attribute__((copy(v)));
int w __attribute__((u9));
int z __attribute__((copy(w)));
#define NO(n, e, a) extern char n[__builtin_has_attribute (e, a) ? -1 : 1]
NO(c1, y, packed);
NO(c2, z, packed);
