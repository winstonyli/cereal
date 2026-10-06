// flags: -Wrestrict
void f(int *x, int *__restrict y);
void g(char *__restrict a, const char *__restrict b, int n);
void h(int *__restrict a, int *__restrict b, int *c);
int arr[10]; char s[20];
void t(int a, int *p, int *q) {
  f(&a, &a);
  f(p, p);
  f(p, q);
  f(arr, arr);
  f(arr, arr + 1);
  f(arr, &arr[0]);
  g(s, s, 1);
  g(s, s + 2, 1);
  h(p, q, p);
  h(p, p, q);
  f(&a, (int *)&a);
  f(p + 1, p + 1);
  f(p, p + 0);
  f(p + 1, p);
}
