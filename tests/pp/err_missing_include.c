/* In -E a missing header ends the TU (GCC: fatal, "compilation terminated"). */
int before;
#include "does_not_exist.h" /* expect: error */
int after_is_not_output;
#error not reached
