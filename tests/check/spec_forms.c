// flags: -Wall -Wextra
typedef unsigned long size_t;
int abs_(int x)
{
#pragma cereal ensures return >= 0 && (return == x || return == -x)
  return x < 0 ? -x : x;
}
void copy(char *dst, const char *src, size_t n)
{
#pragma cereal requires $len(dst) >= n && $len(src) >= n
#pragma cereal ensures dst[0:n] == src[0:n]
  for (size_t i = 0; i < n; i++) {
#pragma cereal invariant i <= n && dst[0:i] == src[0:i]
    dst[i] = src[i];
  }
}
int arr(int a[10], int n)
{
#pragma cereal requires $len(a) == 10 && n >= 0 && n <= 10
#pragma cereal ensures a[0:n] != a[1:n]
  return n;
}
