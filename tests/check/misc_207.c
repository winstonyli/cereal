// flags: -Wall
int printf(const char *, ...);
const char unt[3] = "abc";
const char ok[4] = "abc";
const char pad[8] = "abc";
void f(void) { printf(unt); printf(ok); printf(pad); printf(unt + 1); }
