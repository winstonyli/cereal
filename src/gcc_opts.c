/* gcc_opts.c - what gcc-13 knows about its own -W options. */
#include "gcc_opts.h"

#include <string.h>

#include "c/fuzzy.h"
#include "gcc_pragma_opts.h"
#include "gcc_wopts.h"

/* Is name (a -W option without the -W, any no-/error= prefix and value
 * stripped) one of gcc's?  A valued option is looked up as name=. */
bool gcc_wopt_known(const char *name, bool *valued)
{
    size_t k, n = strlen(name);
    for (k = 0; k < sizeof gcc_wopts / sizeof *gcc_wopts; k++) {
        const char *g = gcc_wopts[k];
        size_t gl = strlen(g);
        if (gl > 1 && g[gl - 1] == '-' && n >= gl && !strncmp(g, name, gl)) {
            *valued = false;    /* a joined form: -Wlarger-than-32768 */
            return true;
        }
        if (gl == n && !strcmp(g, name)) {
            *valued = false;
            return true;
        }
        if (gl == n + 1 && g[n] == '=' && !strncmp(g, name, n)) {
            *valued = true;
            return true;
        }
    }
    return false;
}

bool gcc_wopt_exact(const char *name)
{
    size_t k;
    for (k = 0; k < sizeof gcc_wopts / sizeof *gcc_wopts; k++)
        if (!strcmp(gcc_wopts[k], name))
            return true;
    return false;
}

const char *gcc_wopt_suggest(const char *flag)
{
    Best b;
    uint64_t work = 0;
    size_t k;
    best_init(&b, flag, &work);
    for (k = 0; k < sizeof gcc_wopts / sizeof *gcc_wopts; k++)
        best_consider(&b, gcc_wopts[k]);
    return best_get(&b);
}

static bool in_notwarn(const char *s)
{
    size_t k;
    for (k = 0; k < sizeof pragma_notwarn / sizeof *pragma_notwarn; k++)
        if (!strcmp(pragma_notwarn[k], s))
            return true;
    return false;
}

int gcc_pragma_opt(const char *opt, const char **langs)
{
    const char *s = opt + 1, *eq;
    char name[160];
    size_t n, k;
    bool valued;
    if (opt[0] != '-' || !s[0])
        return PO_UNKNOWN;
    eq = strchr(s, '=');
    if (in_notwarn(s))
        return PO_NOT_WARNING;
    if (eq) {
        n = (size_t)(eq - s) + 1;           /* name= */
        if (n < sizeof name) {
            memcpy(name, s, n);
            name[n] = 0;
            if (in_notwarn(name))
                return PO_NOT_WARNING;
        }
    }
    if (s[0] != 'W')
        return PO_UNKNOWN;
    s++;
    n = eq ? (size_t)(eq - s) : strlen(s);
    if (n >= sizeof name)
        return PO_UNKNOWN;
    memcpy(name, s, n);
    name[n] = 0;
    if (!gcc_wopt_known(name, &valued) || (eq && !valued && !gcc_wopt_exact(name)))
        return PO_UNKNOWN;
    if (!eq && valued && !gcc_wopt_exact(name)) {
        /* a valued option without its value: gcc looks the bare name up */
    }
    for (k = 0; k < sizeof pragma_nonc / sizeof *pragma_nonc; k++) {
        size_t m = strlen(pragma_nonc[k].name);
        bool v = m && pragma_nonc[k].name[m - 1] == '=';
        if ((v ? m - 1 : m) == n && !strncmp(pragma_nonc[k].name, name, n)) {
            if (langs)
                *langs = pragma_nonc[k].langs;
            return PO_OTHER_LANG;
        }
    }
    return PO_WARNING;
}
