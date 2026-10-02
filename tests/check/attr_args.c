// flags: -std=gnu99
int i;
void *f1 (void) __attribute__((assume_aligned (i)));
void *f2 (void) __attribute__((assume_aligned (15)));
void *f3 (void) __attribute__((assume_aligned (16, i)));
void *f4 (void) __attribute__((assume_aligned (16, 32)));
void *f5 (void) __attribute__((assume_aligned (16, -1)));
void *f6 (void) __attribute__((assume_aligned (0)));
void *f7 (void) __attribute__((assume_aligned (1.5)));
int f8 (void) __attribute__((assume_aligned (16)));
void *f9 (void) __attribute__((assume_aligned (1ULL<<40)));
void *f10 (void) __attribute__((assume_aligned ("x")));
void *f11 (void) __attribute__((assume_aligned (-16)));
int v __attribute__((assume_aligned (16)));

int sx __attribute__ ((strict_flex_array (1)));

int [[gnu::strict_flex_array(1)]] sy;

struct trailing {
    int a;
    int c __attribute ((strict_flex_array));
};

struct trailing_1 {
    int a;
    int b;
    int c __attribute ((strict_flex_array (2)));
};

extern int d;

struct trailing_array_2 {
    int a;
    int b;
    int c[1] __attribute ((strict_flex_array (d)));
};

struct trailing_array_3 {
    int a;
    int b;
    int c[0] __attribute ((strict_flex_array (5)));
};
