// flags: -Wunused -Wempty-body
typedef short unused_type __attribute__ ((unused));
typedef unused_type again;
void f (int c)
{
  unused_type y;
  again z;
  short x;
  if (c)
    a: ;
  else
    b: ;
}
