/* indirect recursion through the macro graph
   cereal-flags: -Wmacro-recursion -Wno-unused-macros */
#define PING(x) PONG(x) /* expect: macro-recursion */
#define PONG(x) PING(x)
#define A1 A2 /* expect: macro-recursion */
#define A2 A3
#define A3 (A1 + 1)
#define CHAIN1 CHAIN2
#define CHAIN2 3
#define SELF SELF
#define GONE1 GONE2
#define GONE2 GONE1
#undef GONE2
int x = PING(1) + A1 + CHAIN1 + SELF;
