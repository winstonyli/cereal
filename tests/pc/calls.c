/* calls are leaves: a feature test or function-like macro is one opaque atom,
 * the rest of the condition stays exact */
#if defined(A) && __has_attribute(noreturn)
int a;
#elif __has_attribute(noreturn)
int b;
#endif
#if FOO(1, (2)) || defined(B)
int c;
#endif
