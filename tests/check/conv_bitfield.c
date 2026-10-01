// flags: -Wconversion
struct ub { unsigned int x:1; unsigned y:3; } ub;
struct sb { int x:1; int z:5; } sb;
int bar, bar2; unsigned u; char c;
void f(void)
{
    ub.x = (bar != 0);
    ub.x = bar != 0 ? 1 : 0;
    ub.x = bar;
    ub.x = bar != 0 ? 0 : -1;
    ub.y = 9;
    ub.y = u;
    ub.y = c;
    sb.x = 1;
    sb.x = (bar != 0);
    sb.x = bar != 0 ? 1 : 0;
    sb.x = !bar;
    sb.z = 40;
    sb.z = bar;
    sb.z = (char)bar;
    sb.z = ub.y;
}
