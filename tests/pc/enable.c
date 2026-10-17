/* enable and cover: the host is gcc on linux (enable.flags) */
#ifdef FEATURE_A
int a;                  /* -DFEATURE_A */
#endif
#if defined(FEATURE_A) && FEATURE_B >= 2
int b;                  /* -DFEATURE_A -DFEATURE_B=2, shared with the group above */
#endif
#ifdef FEATURE_C
int c;                  /* -DFEATURE_C */
#endif
#ifdef _WIN32
int w;                  /* a target (os = windows): no overlay */
#endif
#ifdef GUARD_X
int early;              /* the unit defines GUARD_X below: -DGUARD_X would skip the block, so no overlay */
#endif
#ifndef GUARD_X
#define GUARD_X
int g;
#endif
#if defined(__STDC_VERSION__) && __STDC_VERSION__ < 201112L
int s;                  /* -std=gnu99 */
#endif
#if defined(FEATURE_B) && FEATURE_B < 0
int n;                  /* -DFEATURE_B=-1: clashes with -DFEATURE_B=2, so a second overlay */
#endif
