// flags: -std=c11 -pedantic-errors
void *_Atomic apv;
struct s *_Atomic aps;
_Atomic struct t { char c; } as;
int *pi;
_Atomic int *pai;
_Atomic void *pav;
int r;
void f(void)
{
  apv++; --apv; aps++; --aps;
  apv += 1; aps -= 1;
  as += 1; apv *= 1; apv |= 1;
  (void) (r ? pai : pi);
  (void) (r ? pai : pav);
  (void) (r ? pav : pi);
}
