extern int a, b;
int c = &a == &a;
int d = &a != &b;
double atan(double);
double nan(const char *);
double e(double);
const double pi = 4 * atan(1.0);
const double nn = nan("");
const double bad = e(1.0);
