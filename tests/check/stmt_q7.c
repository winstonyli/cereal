// flags: -Wall -Wextra
int g;
void f(int x, char *p, float fl)
{
    switch (x) {
    case (char *)0: break;
    case 1.5: break;
    case g: break;
    case (long)&g: break;
    case 5 ... 4: break;
    case 7 ... 7: break;
    case 7: break;
    case "a": break;
    case (void)0: break;
    case x: break;
    case sizeof(int): break;
    case 1 ? 2 : g: break;
    }
    switch (fl) { case 1: break; }
    switch (p) { case 1: break; }
    switch ((struct {int a;}){0}) { default: break; }
    switch (g) default: ;
}
