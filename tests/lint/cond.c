/* conditional compilation
   cereal-flags: -Wundef -Wcond-constant */
#define FEATURE_A 1
#ifdef FEATURE_A
#  ifndef FEATURE_A /* expect: cond-dead-branch */
#  endif
#  ifdef FEATURE_A /* expect: cond-redundant */
#  endif
#endif
#if defined(X) && !defined(X) /* expect: cond-dead-branch */
#endif
#if VERSION > 2 /* expect: undef */
#elif VERSION > 3 /* expect: cond-dead-branch, undef */
#elif VERSION <= 2 /* expect: cond-redundant, undef */
#else /* expect: cond-dead-branch */
#endif
#if defined(A) || defined(B)
#elif defined(A) /* expect: cond-dead-branch */
#endif
#if !defined(C)
#elif defined(C) /* expect: cond-redundant */
#endif
#if 0 /* expect: cond-constant */
#endif
#ifdef FEATUR_A /* expect: cond-typo */
#endif
#if defined(FEATURE_B)
#endif /* FEATURE_C */ /* expect: endif-label */
#ifdef FEATURE_A
#endif /* FEATURE_A */
#ifdef FEATURE_A
#else /* !FEATURE_A */
#endif
#if UNDEFINED_THING /* expect: undef */
#endif
