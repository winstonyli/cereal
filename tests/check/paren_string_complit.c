// flags: -std=c99 -pedantic
char a[]={("a")};
char *o = (char []){ ("o") };
char *q = (char [2]){ ("o") };
void f(void){char *z = (char []){ ("o") }; char y[]=("x");}
