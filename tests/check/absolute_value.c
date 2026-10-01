// flags: -Wextra
int abs(int); long labs(long); double fabs(double); float fabsf(float);
double cabs(double _Complex); float cabsf(float _Complex);
struct V { unsigned char uc; unsigned ui; int i; double d; long long ll; };
void t(struct V *p, double _Complex *pc)
{
  p->ui = __builtin_abs(p->ui);
  p->ui = 0 ? __builtin_abs(p->ui) : __builtin_abs(p->ui);
  p->d = 0 ? __builtin_fabs(p->d) : __builtin_fabsf(p->d);
  p->i = abs(p->d);
  p->d = fabs(p->i);
  p->d = cabs(p->i);
  p->d = cabsf(*pc);
  p->ll = abs(p->ll);
  p->i = labs(*pc);
}
