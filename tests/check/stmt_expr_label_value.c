// flags: -std=gnu99
void f(void **p, int k)
{
  int a = ({ l1: 1; });
  int b = ({ k; l2: l3: 2; });
  *p = ({ __label__ h; h: &&h; });
  int c = ({ if (k) k; l4: ; });
}
