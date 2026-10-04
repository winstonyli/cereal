// flags: -std=c99 -pedantic -Wall
void b(int);
void f(int i)
{
  switch (i) {
  case 0:
    b(1);
    __attribute__((fallthrough, fallthrough, used));
  case 1:
    b(1);
    __attribute__((used, unused));
  case 2:
    b(1);
    __attribute__((fallthrough)) __attribute__((used));
  case 3:
    b(1);
    __attribute__((fallthrough));
  case 4:
    b(1);
    __attribute__((__fallthrough__));
  case 5:
    b(1);
    __attribute__((fallthrough)) __attribute__((fallthrough));
  case 6:
    b(1);
    __attribute__((fallthrough)) int j;
  case 7:
    b(1);
    __attribute__((fallthrough)) {}
  default:
    b(1);
    __attribute__((used)) int k;
    __attribute__((fallthrough)) ;
    __attribute__((deprecated)) ;
    __attribute__((fallthrough)) b(2);
    ;
  }
  __attribute__((fallthrough));
}
int f2(int i){
 switch(i){
 case 2: __attribute__((used));
 case 3:
   __attribute__((used)) ;
 default: __attribute__((used, unused)); 
 }
  { __attribute__((used)); __attribute__((fallthrough, used)); }
  __attribute__((used)) __attribute__((unused));
 return 0;}
