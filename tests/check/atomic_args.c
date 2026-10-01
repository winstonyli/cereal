enum E;
struct T;
struct T *tp;
enum E *ep;
struct S { int a[3]; };
struct T;
float f;
int i, *ip, arr[3];
_Bool bb;
struct S s;
void h(int n)
{
  __atomic_load_n(&f, 0);
  __atomic_load_n(i, 0);
  __atomic_load_n (  ip  , 0);
  __atomic_load_n(&s, 0);
  __atomic_load_n((struct T *)0, 0);
  __atomic_fetch_add(&bb, 1, 0);
  __sync_fetch_and_add(&bb, 1);
  __sync_fetch_and_add(&f, 1);
  __sync_bool_compare_and_swap(&bb, 1, 2);
  __atomic_load(&i, &i, 0);
  __atomic_load(i, &i, 0);
  __atomic_load((void *)0, &i, 0);
  __atomic_load((struct T *)0, &i, 0);
  __atomic_load(&i, i, 0);
  __atomic_load(&i, &f, 0);
  __atomic_load(&i, (long *)0, 0);
  __atomic_load(&i, &i, f);
  __atomic_load(&i, &i, 99);
  __atomic_store(&i, (const int *)0, 0);
  __atomic_load(&i, (const int *)0, 0);
  __atomic_load(&i, (volatile int *)0, 0);
  __atomic_exchange(&i, &i, &i, 0);
  __atomic_exchange(&i, &i, &i);
  __atomic_exchange(&i, &i, (const int *)0, 0);
  int v[n];
  __atomic_load(&v, &i, 0);
  __atomic_load(&arr, &arr, 0);
  __atomic_compare_exchange(&i, &i, &i, 0, 0, 0);
  __atomic_compare_exchange(&i, &i, &i, f, 0, 99);
  __atomic_load_n(&i, 99);
  __atomic_load_n(&i, f);
}
void h3(void)
{
  __atomic_load_n(tp, 0);
  __atomic_load_n(
    ep, 0);
  __atomic_load_n((struct T *)0, 0);
  __atomic_load_n(  (struct T *)0, 0);
  __atomic_load_n((enum E *)tp, 0);
  __sync_fetch_and_add((struct T*)tp, 1);
}
_Atomic int ai;
long al;
void h4(void)
{
  __builtin_add_overflow(1, 2, &ai);
  __builtin_sub_overflow(1, 2, &al);
}
