// flags: -Wmisleading-indentation
int flagA, flagB, cnt, n;
void a1(void)
{
  { for (cnt = 0; cnt < n; ++cnt)
    if (flagA)
      break;
    n = 1; }
}
void a2(void)
{
  int x = ({ for (cnt = 0; cnt < n; ++cnt)
    if (flagA)
      break;
    n = 1; n; });
}
void a3(void)
{
  int x = ({ for (cnt = 0; cnt < n; ++cnt)
              if (flagA)
                break;
              n = 1; n; });
}
void a4(void)
{
  int x = ({ for (cnt = 0; cnt < n; ++cnt)
              flagA = 1;
              n = 1; n; });
}
void a5(void)
{
  int x = ({ for (cnt = 0; cnt < n; ++cnt)
              flagA = 1;
              n; });
}
void
fn_40_a (const char *end, const char *thousands, int thousands_len)
{
  int cnt;

  while (flagA)
    if (flagA
        && ({ for (cnt = 0; cnt < thousands_len; ++cnt)
              if (thousands[cnt] != end[cnt])
                break;
              cnt < thousands_len; })
        && flagB)
      break;
}
