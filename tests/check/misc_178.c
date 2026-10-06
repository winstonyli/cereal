void bar (char *) __attribute__((constructor(foo)));
void b2 (void) __attribute__((destructor(foo, 3)));
void b3 (void) __attribute__((constructor(70000)));
void b4(void) __attribute__((constructor(101)));
void b5 (void) __attribute__((destructor(1, 3)));
