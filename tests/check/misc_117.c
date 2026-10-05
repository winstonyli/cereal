// flags: -Wchar-subscripts
/* A designated string for a flexible array member that is stored out of order
 * is digested again at the end, reported at the last tag of the declaration.
 * -Wchar-subscripts is located at the '['.  */

struct s { int a; char b[]; };
struct s d = { .b = "" };
struct s e = { .b = "x", };
struct s f = { .a = 1, .b = "x" };
struct s g = { 1, "x" };
struct s h = { .b = { 1 } };

extern int arr[];
char ch;

int sub(void)
{
    return arr[ch] + ch[arr];
}
