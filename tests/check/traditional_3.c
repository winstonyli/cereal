// flags: -Wtraditional
void f(void)
{
  { int a[2] = { 1, 2 }; }
  {    int a[2] = { 1, 2 }; }
  { int b; int a[2] = { 1, 2 }; }
  int c; int a[2] = { 1, 2 };
  { char s[] = "abc"; }
  { const int a[2] = { 1, 2 }; }
  { unsigned a[2] = { 1, 2 }; }
  { signed char a[2] = { 1, 2 }; }
  { long long a[2] = { 1, 2 }; }
}
