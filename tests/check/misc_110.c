/* A parameter that cannot start a declaration is diagnosed, then the list goes
 * on after a following comma (so the next parameter is diagnosed too). */
const int jc;
extern int j;
extern typeof(0,jc) j;
extern typeof(+jc) j;
void g(1, int a, 2);
