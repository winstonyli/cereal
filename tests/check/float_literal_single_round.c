// flags: -std=gnu11 -Wno-pedantic
/* A floating literal is rounded once, from its text to its type: through a
 * 64-bit mantissa first it would round twice and land on the wrong side of a
 * tie. */
_Static_assert(9007199254740993.00000001 == 9007199254740994.0, "double above a tie");
_Static_assert(9007199254740993.0 == 9007199254740992.0, "double on a tie, to even");
_Static_assert(16777217.00000001f == 16777218.0f, "float above a tie");
_Static_assert(16777217.0f == 16777216.0f, "float on a tie, to even");
_Static_assert(0x1.00000000000008p0 == 1.0, "hex double on a tie");
_Static_assert(0x1.000000000000081p0 > 1.0, "hex double above a tie");
