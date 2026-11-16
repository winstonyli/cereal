/* Straight-line code with a branch: both paths establish the postcondition. */
int clampz(int x)
{
#pragma cereal ensures return >= 0
    if (x < 0)
        return 0;
    return x;
}
