// flags: -Wbidi-chars=any
int \U0000003b;
int a\u007fb;
int x\u00d8;
const char *a1 = "\u001f";
const char *a2 = "\u0024";
const char *a3 = "\U80000000";
const char *a4 = "\u{41}";
const char *a5 = "\N{LEFT-TO-RIGHT OVERRIDE}x";
