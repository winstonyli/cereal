/* A product with a 64-bit operand has no exact 64-bit image: reported, never guessed. */
int area(int *a, unsigned long n)
{
#pragma cereal requires n * n <= $len(a)
    return 0;
}
