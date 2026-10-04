/* gcc_opts.h - what gcc-13 knows about its own -W options, shared by the
 * driver (unknown -W options) and the checker (#pragma GCC diagnostic). */
#ifndef CEREAL_GCC_OPTS_H
#define CEREAL_GCC_OPTS_H

#include <stdbool.h>

/* Is name (a -W option without the -W, any no-/error= prefix and value
 * stripped) one of gcc's?  A valued option is looked up as name=. */
bool gcc_wopt_known(const char *name, bool *valued);
/* Whether name is in the table verbatim (a valued option: with its =), 
 * no value form tried. */
bool gcc_wopt_exact(const char *name);
/* The closest gcc -W option to flag (without the -W), or NULL. */
const char *gcc_wopt_suggest(const char *flag);

/* How #pragma GCC diagnostic treats the option string opt (with its
 * leading '-'): */
enum { PO_WARNING, PO_UNKNOWN, PO_OTHER_LANG, PO_NOT_WARNING };
int gcc_pragma_opt(const char *opt, const char **langs);

#endif
