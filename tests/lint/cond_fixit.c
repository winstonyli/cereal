/* cond-typo fix-it (docs/C_DESIGN.md 4.2, Q1): INFO, the name may be defined
   in another configuration. */
#define FEATURE_A 1
#ifdef FEATUR_A /* expect: cond-typo */ /* expect-fixit: 4:8-4:16 "FEATURE_A" info */
#endif
#if defined(FEATURE_AA) /* expect: cond-typo */ /* expect-fixit: 6:13-6:23 "FEATURE_A" info */
#endif
int v = FEATURE_A;
