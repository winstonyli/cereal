// flags: -Wall
/* docs/ATTR_IDS_DESIGN.md golden (b): attr_excl_generic keeps at most 24 attributes of a declaration,
 * unknown ones included, so after 24 unknown names the later `pure` is never recorded and the
 * `noreturn` that excludes it prints no conflict. */
__attribute__((u01, u02, u03, u04, u05, u06, u07, u08, u09, u10, u11, u12,
               u13, u14, u15, u16, u17, u18, u19, u20, u21, u22, u23, u24,
               pure, noreturn)) int f(void);
