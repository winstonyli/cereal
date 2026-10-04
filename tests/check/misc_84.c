typedef int user_int;
typedef user_int user_int_copy;
struct s { int a; };
user_int (*__attribute__((__transaction_unsafe__)) unsafe_p1)(void);
user_int_copy (*__attribute__((__transaction_unsafe__)) unsafe_p2)(void);
int (*__attribute__((transaction_unsafe)) unsafe_p3)(void);
int (*plain_p)(void);
int (*(*__attribute__((transaction_unsafe)) pp)(void))(int);
void f(void)
{
    struct s s;
    unsafe_p1 = &s;
    unsafe_p2 = &s;
    unsafe_p3 = plain_p;
    plain_p = unsafe_p3;
    pp = 1.0;
}
