// trap: computed goto through a null pointer
int main(void)
{
    void *p = 0;
    goto *p;
    return 0;
}
