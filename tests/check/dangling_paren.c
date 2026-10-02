// flags: -std=gnu99 -Wparentheses
void f(int a, int b) { if (a) if (b) a = 1; else a = 2; }
