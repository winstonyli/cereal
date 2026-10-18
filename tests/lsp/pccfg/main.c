static int featured(void) { return 1; }
static int deep_fn(void) { return 2; }
#if FEATURE_X
int fx = featured();
#endif
#if LEVEL >= 3
int dp = deep_fn();
#endif
#ifdef TOGGLE_ON
int on;
#endif
#ifndef TOGGLE_OFF
int off;
#endif
