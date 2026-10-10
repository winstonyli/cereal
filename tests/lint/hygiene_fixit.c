/* macro hygiene fix-its (docs/C_DESIGN.md 4.1): the parameter is wrapped, and
   the body of an expression macro; the body gets no fix when it holds a
   top-level comma.
   cereal-flags: -Wmacro-unused-param */
#define SUM 1 + 2 /* expect: macro-unparenthesized-body */ /* expect-fixit: 5:13-5:13 "(" safe */ /* expect-fixit: 5:18-5:18 ")" safe */
#define NEG(x) -x /* expect: macro-unparenthesized-param */ /* expect-fixit: 6:17-6:18 "(x)" safe */
#define SQ(x) x * x /* expect: macro-unparenthesized-param, macro-unparenthesized-body */ /* expect-fixit: 7:15-7:16 "(x)" safe */ /* expect-fixit: 7:19-7:20 "(x)" safe */ /* expect-fixit: 7:15-7:15 "(" safe */ /* expect-fixit: 7:20-7:20 ")" safe */
#define OK ((1) + (2))
int use(void) { return SUM + NEG(1) + SQ(2) + OK; }
