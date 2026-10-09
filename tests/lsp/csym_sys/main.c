#include <fakelib.h>
int main(void)
{
    unsigned long n = fake_len("abc");
    fake_printf("%lu", n);
    return 0;
}
