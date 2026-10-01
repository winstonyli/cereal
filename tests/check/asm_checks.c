// flags: -std=gnu99
const int ci = 0;
void f(int *p, void *v, const int cp)
{
    register int r;
    int i;
    asm volatile volatile ("" :::);
    asm goto goto ("" :::: l);
    asm("" : "=r" ((char)i));
    asm("" : "=m" (ci));
    asm("" : "=r" (cp));
    asm("" : : "m" (r));
    asm("" : : "r" (*v));
    asm("" : [a] "=r" (i) : [a] "r" (i));
    asm("%[nope]" :: [x] "r" (i));
    asm("" : "r" (i));
    asm("" :: "\n" (i));
    asm("%[x]" :: [x] "r" (i));
    asm("" : "=m" (*p) : "g" (r));
l:;
}
