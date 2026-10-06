// flags: -Wmisleading-indentation
int flagA, cnt, n;
void b1(void)
{
  { for (cnt = 0; cnt < n; ++cnt)
      flagA = 1;
      n = 1; }
}
void b2(void)
{
  { for (cnt = 0; cnt < n; ++cnt)
    flagA = 1;
    n = 1; }
}
void b3(void)
{
  { for (cnt = 0; cnt < n; ++cnt)
        flagA = 1;
        n = 1; }
}
void b4(void)
{
  if (n) { for (cnt = 0; cnt < n; ++cnt)
      flagA = 1;
      n = 1; }
}
void b5(void)
{
  n = 2; for (cnt = 0; cnt < n; ++cnt)
         flagA = 1;
         n = 1;
}
void b6(void)
{
  n = 2; for (cnt = 0; cnt < n; ++cnt)
    flagA = 1;
    n = 1;
}
