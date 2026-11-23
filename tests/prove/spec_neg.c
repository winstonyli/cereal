/* Unary minus on an int in a spec is exact in 64 bits (-INT_MIN included): accepted. */
int neg(int x)
{
#pragma cereal requires x >= 0
#pragma cereal ensures -x <= 0
    return x;
}
