// flags: -Wdeclaration-after-statement
#include <stdbool.h>
int k (int [sizeof &&z]);
void *p = &&w;
int q = (int)&&w;
void a1 () {}
void a1 () = 0;
void a2 ();
void a2 () = 0;
void a3 () = 0;
void a4 () {}
void a4 () = 1+2;
void a5 () {}
int ds(char *p){
  if (!p)
    return 0;
  bool r = true;
  return r;
}
