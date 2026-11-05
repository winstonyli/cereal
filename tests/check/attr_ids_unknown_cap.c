// flags: -Wall
/* Pins the set machinery of docs/ATTR_IDS_DESIGN.md, golden (a): a declaration whose only attribute
 * is unknown still owns an attribute set, so the 3-slot cexpr_asets (typedef chain of two sets,
 * then x's own set) is full before the record's `packed` is reached.  This is cereal's cap, not
 * gcc's (gcc has no such cap); see "Found" in the design. */
struct __attribute__((packed)) P { char c; int i; };
typedef struct P T1 __attribute__((u1));
typedef T1 T2 __attribute__((u2));
T2 x __attribute__((u3));
#define NO(e, a) extern char chk[__builtin_has_attribute (e, a) ? -1 : 1]
NO(x, packed);
