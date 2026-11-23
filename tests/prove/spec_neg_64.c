/* Unary minus on a 64-bit operand in a spec has no exact 64-bit image either (-LONG_MIN wraps): reported, never guessed. */
int area(int *a, long n)
{
#pragma cereal requires -n <= 0
    return 0;
}
