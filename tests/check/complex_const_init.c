// flags: -std=c11 -pedantic-errors
_Complex double c = __builtin_complex (0.0, __builtin_nan (""));
_Complex double e = __builtin_complex (0.0, __builtin_inf ());
_Complex float g = __builtin_complex (1.0f, 1.0f) / __builtin_complex (0.0f, __builtin_nanf (""));
