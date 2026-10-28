// trap: computed goto to an address that is not a label of the function
int g;
int main(void)
{
    void *p = &g;
    goto *p;
    return 0;
}
