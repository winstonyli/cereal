#define h h>
#include <stddef.h
#undef h
#define foo stddef.h>
#include <foo
size_t n;
