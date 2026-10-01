typedef int IA[];
IA a0;
void f(void) {
    extern IA a0;
    sizeof(a0);
}
