void f1(void) __attribute__((visibility("hidden")));
void f1(void) __attribute__((visibility("default")));
void f2(void) __attribute__((visibility("hidden")));
void f2(void) __attribute__((__visibility__("hidden")));
void f3(void) __attribute__((visibility("hidden")));
void f3(void);
void f4(void) __attribute__((visibility("hidden")));
void f4(void) {}
void f4(void) __attribute__((visibility("internal")));
int v1 __attribute__((visibility("protected")));
int v1 __attribute__((visibility("hidden"))) = 1;
extern int v2;
int v2 __attribute__((visibility("hidden")));
