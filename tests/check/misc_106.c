// flags: -std=gnu99
/* A statement expression whose value is a call of an implicitly declared
 * function is located at its brace, not at the call. */
void g1(void){
    ({ f(); })();
    ({ x; f(); })();
    ({ (f()); })();
}
void g1(void){
    ({ f(); })();
}
void h2(void){
    ({ f(); })();
}
