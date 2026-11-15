int c1(int n)
{
#pragma cereal /* c */ requires n > 0
  return n;
}
int c2(int n)
{
#pragma cereal \
 requires n > 0
  return n;
}
int ok(int n)
{
#pragma cereal requires n > 0
  return n;
}
