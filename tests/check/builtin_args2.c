// flags: -std=gnu99
// argument checks of __builtin_clear_padding, __atomic_is_lock_free,
// __builtin_speculation_safe_value; pointer difference of an empty aggregate;
// the address of a bit-field as an asm memory operand
struct S;
struct T { char a; long long b; };
struct B { int a : 1; };
void foo (struct S *p, void *q, char *r, const struct T *s, int *a, double b, int x, struct B *bp)
{
  __builtin_clear_padding ();
  __builtin_clear_padding (1);
  __builtin_clear_padding (&p, 1);
  __builtin_clear_padding (p);
  __builtin_clear_padding (q);
  __builtin_clear_padding (r);
  __builtin_clear_padding (s);
  __atomic_is_lock_free (2, a, 2);
  __atomic_is_lock_free (2);
  __atomic_is_lock_free (2, b);
  __atomic_always_lock_free (2, 0);
  __atomic_is_lock_free (2, x);
  x = x == __builtin_speculation_safe_value ();
  x = 1 + __builtin_speculation_safe_value (1, 2, 3);
  __asm__ ("" : "+m" (bp->a));
  __asm__ ("" : : "m" (bp->a));
}
__PTRDIFF_TYPE__ pd (int p[3][0], int q[3][0]) { return p - q; }
