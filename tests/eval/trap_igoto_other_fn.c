// trap: computed goto to an address that is not a label of the function
static void *addr;
static void f(void)
{
    addr = &&l;
l:  return;
}
int main(void)
{
    f();
    goto *addr;
    return 0;
}
