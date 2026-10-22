// trap: through a pointer to no object
/* A pointer made of other bytes names no object: a trap, not a crash. */
int main(void)
{
    unsigned long long raw = 0xfffffffb00000002ull;
    int *p;
    __builtin_memcpy(&p, &raw, sizeof p);
    return *p;
}
