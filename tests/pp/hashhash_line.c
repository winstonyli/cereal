/* A line starting with ## (or %:%:, or a spliced ##) is text, not a
 * directive: in active groups, in skipped groups, and after directives. */
#if 0
##else
skipped
#endif
a
#if 1
%:%:else
b
#endif
#\
#x
c
#define M 1
##y
d
#if 0
%:%:endif
skipped
#else
e
#endif
