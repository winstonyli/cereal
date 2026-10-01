int i; _Bool b; const volatile long cl;
void t(int *p, long *q, _Bool *bp)
{
  __typeof(__sync_fetch_and_add(p, 1)) x1 = 0; int *a1 = &x1;
  __typeof(__atomic_load_n(q, 0)) x2; int *a2 = &x2;
  __typeof(__sync_bool_compare_and_swap(p, 1, 2)) x3; int *a3 = &x3;
  __typeof(__sync_lock_release(p)) *x4;
  __typeof(__atomic_compare_exchange_n(p, p, 1, 0, 0, 0)) x5; int *a5 = &x5;
  __typeof(__atomic_add_fetch(p, 1, 0)) x6; char *a6 = &x6;
  __typeof(__atomic_load_n(&cl, 0)) x7; char *a7 = &x7;
  __typeof(__atomic_exchange_n(bp, 1, 0)) x8; char *a8 = &x8;
}
