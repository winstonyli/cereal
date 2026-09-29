/* #pragma GCC poison applies from the pragma on: uses before it are fine,
 * also when parallel workers run the earlier text after phase A has seen
 * the pragma. */
int early_use = foo + bar;
int more_text_so_the_parallel_run_splits_here = 1;
int and_some_more_text_to_fill_another_chunk = 2;
#pragma GCC poison foo
#define bar 3
int after = bar;
int late_use = foo; /* expect: error */
_Pragma("GCC poison bar") /* expect: error */
int later = bar; /* expect: error */
#ifdef bar /* expect: error */
#error poisoning drops the definition
#endif
#define BODY foo /* expect: error */
#define EARLY_BODY_OK early
#pragma GCC poison early
EARLY_BODY_OK
#define STR(x) #x
STR(foo) /* expect: error */
#if 0
foo bar
#elif defined(foo) /* GCC does not check #elif */
#elif foo
#else
#endif
#pragma GCC poison foo
