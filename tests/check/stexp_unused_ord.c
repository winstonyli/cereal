// flags: -Wunused-variable -Wformat
void f(void)
{
  int s = ({ int n = 0; 1; });
  (void) s;
}
