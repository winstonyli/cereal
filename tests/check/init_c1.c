// flags: -Wall -Wextra
int *p = (int[]){1,2,3};
int main(void){
  int *q = (int[]){1,2,3,4};
  int n = sizeof((int[]){1,2,3});
  char *s = (char[]){"abc"};
  int *e = (int[]){};
  struct S *z = (struct S){1};
  int m[] = (int[]){1,2};
  return n;
}
