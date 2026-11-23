/* Unary minus on a 64-bit operand in a spec is exact Int: -n > 0 holds for every n < 0, LONG_MIN included (a 64-bit -LONG_MIN wraps to itself). */
long negate(long n)
{
#pragma cereal requires n < 0
#pragma cereal ensures -n > 0
    return n;
}
