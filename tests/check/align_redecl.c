int c __attribute__((aligned (2)));
_Static_assert (_Alignof (c) == 2, "var aligned below its type");
typedef int T;
typedef int T __attribute__((aligned (64)));
typedef int T __attribute__((aligned (128)));
typedef int T __attribute__((aligned (64)));
_Static_assert (_Alignof (T) == 128, "typedef redeclaration only raises");
_Static_assert (_Alignof (T) == 64, "expected to fail");
