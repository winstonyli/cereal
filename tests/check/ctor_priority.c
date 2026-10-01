void a(void) __attribute__((constructor(50)));
void b(void) __attribute__((destructor(70000)));
void c(void) __attribute__((constructor(-1)));
void d(void) __attribute__((constructor(500)));
