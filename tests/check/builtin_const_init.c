// flags: -Wpedantic
double atan(double);
double nan(const char *);
const double lib = 4*atan(1.0);       /* pedwarn: library call folded */
const double lib2 = nan("");
const double ok = 4*__builtin_atan(1.0);   /* true constant */
const double ok2 = __builtin_nan("");
const double ok3 = __builtin_inf();
int tab	= 1 +	"x";
