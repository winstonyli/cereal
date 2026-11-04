// flags: -Wall
/* a call statement whose result is unused: a pure built-in warns, a user
 * function that merely shares no name with one does not */
int user_fn(int);

void f(int a)
{
    user_fn(a);
    __builtin_abs(a);
}
