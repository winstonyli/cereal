/* __builtin_tgmath selection and _FloatN arithmetic conversions */
extern float f32(float); extern double f64(double); extern long double fl(long double);
#define T(x) __builtin_tgmath(f32, f64, fl, x)
float a; double b; long double c; int i;
float ra(void) { return T(a); }
double rb(void) { return T(b); }
long double rc(void) { return T(c); }
double ri(void) { return T(i); }
#ifdef __FLT32X_MAX__
_Float32x x; _Float64 y;
_Float64 u(void) { return x + y; }
#endif
