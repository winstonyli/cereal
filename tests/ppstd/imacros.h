/* -imacros: only the macros survive; the tokens and the __COUNTER__ uses are dropped */
#define FROM_IMACROS 1
#define TWICE(x) ((x) + (x))
#include "imacros_nested.h"
int leaked_declaration = __COUNTER__;
#ifdef Q
int leaked_under_q;
#endif
#define SAW_Q_IN_IMACROS defined_below
