void f(int x)
{
    __asm__("" : "=r"(x) : " r"(x));              /* space: input only */
    __asm__("" : "= r"(x));                       /* output: accepted */
    __asm__("" : "=r"(x) : "\t"(x));
    __asm__("" : "=(-)"(x));                      /* output: any punctuation */
    __asm__("" : : "0"(x));                       /* no such output */
    __asm__("" : "=r"(x) : "1"(x));
    __asm__("" : "=r"(x) : "10"(x));
    __asm__("" : "=r"(x) : "0"(x));
    __asm__("" : "=0"(x));
    __asm__("" : "=[a]"(x));
    __asm__("" : [a] "=r"(x) : "[a]"(x));
    __asm__("" : [a] "=r"(x) : [b] "[b]"(x));     /* an input name */
    __asm__("" : [a] "=r"(x) : [b] "[a]"(x));
    __asm__("" : "=r"(x) : "[y]"(x));
    __asm__("" : "=r"(x) : "r[x"(x));
}
