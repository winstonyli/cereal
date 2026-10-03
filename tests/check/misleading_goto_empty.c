// flags: -Wmisleading-indentation
void foo(int);
int a, b;
#define BAR() foo (0)
void t(void)
{
  if (a)
    foo (1);
    BAR();
  if (a)
    BAR();
    foo (2);
}


void t2(void)
{
  while (a);
    foo (0);
  while (a);
  foo (0);
  while (a)
    ;
    foo (1);
  while (a)
    ;
  foo (1);
  for (;;) ;
      foo (2);
  if (a);
\tfoo (3);
  if (a) ; else if (b);
    foo (4);
    if (a);
  foo (5);
  if (a)
    ;
    ;
  while (a);
      ;
  foo (6);
}
void u(void)
{
  if (a);
      foo (7);
  for (;;)
    ;
      foo (8);
}

int fn2(int a){
	if (a)
		a = 1;
		a = 2;
	if (a)
		return 1;
		return 2;
}
int g2(int a){
	if (a)
		goto f;
		goto f;
f: return 0;}
