/* float folds round once, in the type's own precision (AR8).  Every array has one element when the fold
 * matches the hardware: gcc-13 at run time gives 1+2^-52 for the first sum, 2^53 for the second, 1+2^-23 for the float sum. */
int d1[(1.0 + 0x1.0000002p-53 == 1.0) ? -1 : 1];
int d2[(1.0 + 0x1.0000002p-53 == 0x1.0000000000001p0) ? 1 : -1];
int d3[(1.0 + 9007199254740993 == 9007199254740992.0) ? 1 : -1];
int d4[(1.0f + 0x1.000002p-24f == 0x1.000002p0f) ? 1 : -1];
