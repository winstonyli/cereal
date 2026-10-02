/* incomplete operands: require_complete_type at gcc's locations */
enum E e;
extern enum F ve;
extern struct S vs;
int a[10];
void bar(int [e]);
int g(int i)
{
  int x = a[ve];
  ve;
  (void) vs;
  (void) (i * ve);
  (void) (ve ? 1 : 1);
  (void) (ve = ve);
  i = ve;
  if (ve)
    ;
  while (vs)
    ;
  switch (ve)
    ;
  (void) __alignof (ve);
  (void) __alignof (vs);
  (void) __alignof (a);
  return x + i[ve];
}
