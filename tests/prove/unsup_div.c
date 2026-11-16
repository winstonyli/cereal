/* Division is outside the supported fragment. */
unsigned long half(unsigned long n)
{
#pragma cereal ensures return <= n
    return n / 2;
}
