// flags: -Wall -Wextra
char c; int i; signed char sc;
int f(void){ int r=0;
r += c < 0;
r += c <= 127;
r += sc <= 127;
r += sc < 127;
r += i == sizeof(int);
r += i == 4ul;
return r;}
