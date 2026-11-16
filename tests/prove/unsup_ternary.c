/* A conditional expression is outside the supported fragment: reported, never guessed. */
int pick(int a, int b)
{
#pragma cereal ensures return >= 0
    return a > b ? a : b;
}
