// flags: -std=c11
_Static_assert (__INT_MAX__ * 2, "overflow");
void f(void){ int j=0;
  for (_Static_assert (sizeof (struct s { int k; }), ""); j < 10; j++) ;
  for (_Static_assert (1, ""); j < 10; j++) ;
  for (int z = sizeof (struct s2 { int k; }); j < 10; j++) ;
  for (int z = 0, w = sizeof (struct s3 { int k; }); j < 10; j++) ;
}
