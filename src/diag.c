/* diag.c - diagnostics engine. */
#include "diag.h"
#include "fixset.h"
#include "json.h"
#include "utf8.h"
#include "wcwidth.h"

#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Every configurable diagnostic.  Hard errors use id "" and are not listed. */
static const DiagOption options[] = {
#define DIAG_OPT(id, ...) {__VA_ARGS__},
#define DIAG_TAG(id, name)
#include "diag_opts.def"
#undef DIAG_OPT
#undef DIAG_TAG
};

/* The names of the tags, in enum order after DW_COUNT. */
static const char *const tag_names[] = {
#define DIAG_OPT(id, ...)
#define DIAG_TAG(id, name) name,
#include "diag_opts.def"
#undef DIAG_OPT
#undef DIAG_TAG
};
_Static_assert(sizeof tag_names / sizeof tag_names[0] == DW_END - DW_COUNT - 1, "tag_names and the enum disagree");

#define NOPTIONS (sizeof options / sizeof options[0])
_Static_assert(sizeof options / sizeof options[0] == DW_COUNT - 1, "diag_opts.def and options[] disagree");

/* Umbrella flags (gcc's EnabledBy): -1 unset, 0 off, 1 on. */
enum { U_ALL, U_EXTRA, U_UNUSED, U_IMPLICIT, NUMBRELLA };

/* Warning configuration from -W flags: built once, then shared read-only
 * by every engine (translation units run concurrently).  Explicit flags
 * win over umbrella flags whatever their order, as in gcc. */
struct DiagConfig {
    DiagLevel overrides[NOPTIONS];
    bool overridden[NOPTIONS];
    signed char optlevel[NOPTIONS];  /* -Wfoo=N: N, 0: not given */
    uint64_t optsize[NOPTIONS];      /* -Wfoo=N for a size N, units applied */
    bool error[NOPTIONS];            /* -Werror=X */
    bool noerror[NOPTIONS];          /* -Wno-error=X: not promoted by -Werror */
    signed char umbrella[NUMBRELLA];
    bool everything;
    bool werror;
    bool pedantic;
    signed char conv;        /* -Wconversion given: 1 on, -1 off, 0 unset */
    uint32_t gen;            /* changes with every edit: keys DiagEngine.memo */
};

static uint32_t cfg_gen_next;

DiagConfig *diag_config_new(void)
{
    DiagConfig *c = xcalloc(1, sizeof(DiagConfig));
    memset(c->umbrella, -1, sizeof c->umbrella);
    c->gen = __atomic_add_fetch(&cfg_gen_next, 1, __ATOMIC_RELAXED);
    return c;
}

void diag_config_free(DiagConfig *c)
{
    free(c);
}

DiagConfig *diag_config_clone(const DiagConfig *c)
{
    DiagConfig *n = xcalloc(1, sizeof(DiagConfig));
    if (c)
        *n = *c;
    else
        memset(n->umbrella, -1, sizeof n->umbrella);
    n->gen = __atomic_add_fetch(&cfg_gen_next, 1, __ATOMIC_RELAXED);
    return n;
}

bool diag_config_werror(const DiagConfig *c)
{
    return c && c->werror;
}

bool diag_config_pedantic(const DiagConfig *c)
{
    return c && c->pedantic;
}

/* Does flag (without "no-") name option o?  "X" or "X=N" for a level
 * option "X="; *level gets N (1 when absent). */
static bool name_matches(const DiagOption *o, const char *flag, long *level)
{
    size_t n = strlen(o->name);
    *level = 1;
    if (strcmp(o->name, flag) == 0)
        return true;
    if (!strcmp(flag, "missing-format-attribute"))   /* gcc's alias */
        return !strcmp(o->name, "suggest-attribute=format");
    if (n && o->name[n - 1] == '=') {
        if (strncmp(o->name, flag, n - 1) != 0)
            return false;
        if (!flag[n - 1])
            return true;
        if (flag[n - 1] != '=' || !flag[n])
            return false;
        *level = !strcmp(o->name, "cast-align=") && !strcmp(flag + n, "strict")
                     ? 2 : strtol(flag + n, NULL, 10);
        return true;
    }
    return false;
}

/* gcc's size arguments: digits and an optional unit (kB 1000, KiB 1024, ...). */
static bool parse_size(const char *v, uint64_t *out)
{
    static const struct { const char *u; uint64_t m; } units[] = {
        {"", 1}, {"B", 1}, {"kB", 1000}, {"KB", 1000}, {"KiB", 1024},
        {"MB", 1000000}, {"MiB", 1u << 20}, {"GB", 1000000000},
        {"GiB", 1u << 30}};
    char *e;
    uint64_t n;
    size_t k;
    if (!isdigit((unsigned char)*v))
        return false;
    n = strtoull(v, &e, 10);
    for (k = 0; k < sizeof units / sizeof *units; k++)
        if (!strcasecmp(e, units[k].u)) {
            *out = n * units[k].m;
            return true;
        }
    return false;
}

static long find_index_scan(const char *name)
{
    size_t i;
    long lv;
    for (i = 0; i < NOPTIONS; i++)
        if (name_matches(&options[i], name, &lv))
            return (long)i;
    return -1;
}

/* Option indices sorted by name, built once: an exact name is found by
 * bisection; anything else ("X=N", aliases, unknown) takes the scan. */
static uint16_t by_name[NOPTIONS];
static pthread_once_t by_name_once = PTHREAD_ONCE_INIT;

static int by_name_cmp(const void *a, const void *b)
{
    return strcmp(options[*(const uint16_t *)a].name,
                  options[*(const uint16_t *)b].name);
}

static void by_name_init(void)
{
    size_t i;
    for (i = 0; i < NOPTIONS; i++)
        by_name[i] = (uint16_t)i;
    qsort(by_name, NOPTIONS, sizeof *by_name, by_name_cmp);
}

static long find_index(const char *name)
{
    size_t lo = 0, hi = NOPTIONS;
    pthread_once(&by_name_once, by_name_init);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(options[by_name[mid]].name, name);
        if (!c)
            return by_name[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return find_index_scan(name);
}

DiagOpt diag_opt_find(const char *name)
{
    long i = find_index(name);
    return i < 0 ? DW_NONE : (DiagOpt)(i + 1);
}

const char *diag_opt_name(DiagOpt o)
{
    if (o == DW_NONE)
        return "";
    return o < DW_COUNT ? options[o - 1].name : tag_names[o - DW_COUNT - 1];
}

/* The registry row of a configurable option; -1 for DW_NONE and for tags. */
static long opt_index(DiagOpt o)
{
    return o > DW_NONE && o < DW_COUNT ? (long)o - 1 : -1;
}

/* Every option name finds the same option by bisection as by the scan, and
 * is the enum value of its row. */
int diag_selftest(void)
{
    size_t i;
    int bad = 0;
    for (i = 0; i < NOPTIONS; i++)
        if (find_index(options[i].name) != find_index_scan(options[i].name)) {
            fprintf(stderr, "diag selftest: %s\n", options[i].name);
            bad++;
        }
    for (i = 0; i < NOPTIONS; i++)
        if (diag_opt_find(options[i].name) != (DiagOpt)(i + 1) ||
            strcmp(diag_opt_name((DiagOpt)(i + 1)), options[i].name)) {
            fprintf(stderr, "diag selftest: enum of %s\n", options[i].name);
            bad++;
        }
    if (diag_opt_find("no-such-option") != DW_NONE || diag_opt_name(DW_NONE)[0]) {
        fprintf(stderr, "diag selftest: DW_NONE\n");
        bad++;
    }
    return bad != 0;
}

static void by_names(uint8_t by, char *buf)
{
    static const char *const names[] = {"all", "extra", "pedantic", "unused",
                                        "implicit", "unused+extra"};
    size_t k;
    buf[0] = 0;
    for (k = 0; k < sizeof names / sizeof names[0]; k++)
        if (by & (1u << k)) {
            if (buf[0])
                strcat(buf, ",");
            strcat(buf, names[k]);
        }
}

void diag_list_options(FILE *out)
{
    size_t i;
    char by[64];
    for (i = 0; i < NOPTIONS; i++) {
        by_names(options[i].by, by);
        fprintf(out, "  -W%-30s %-8s %-4s %-14s %s\n", options[i].name,
                options[i].group, options[i].on ? "on" : "off", by,
                options[i].help);
    }
}

void diag_init(DiagEngine *d, Arena *a, SrcMgr *sm)
{
    memset(d, 0, sizeof *d);
    d->arena = a;
    d->sm = sm;
    d->out = stderr;
    d->immediate = true;
    d->max_errors = 50;
}

void diag_init_worker(DiagEngine *d, Arena *a, SrcMgr *sm, const DiagEngine *tu)
{
    diag_init(d, a, sm);
    d->cfg = tu->cfg;
    d->werror = tu->werror;
    d->pedantic = tu->pedantic;
    d->pedantic_errors = tu->pedantic_errors;
    d->show_system = tu->show_system;
    d->max_errors = tu->max_errors;
    d->track0 = tu->track0;
}

void diag_free(DiagEngine *d)
{
    size_t i;
    for (i = 0; i < d->all.len; i++) {
        vec_free(&d->all.data[i]->notes);
        vec_free(&d->all.data[i]->fixits);
    }
    vec_free(&d->hdr_seen);
    vec_free(&d->all);
}

bool diag_config_apply(DiagConfig *c, const char *flag)
{
    static const char *const umbrellas[NUMBRELLA] = {"all", "extra", "unused",
                                                     "implicit"};
    bool on = true, err = false;
    char joined[48];
    size_t i;
    c->gen = __atomic_add_fetch(&cfg_gen_next, 1, __ATOMIC_RELAXED);
    bool found = false;
    if (strcmp(flag, "shadow=global") == 0)
        flag = "shadow";                /* gcc: an alias of -Wshadow */
    if (strcmp(flag, "error") == 0) {
        c->werror = true;
        return true;
    }
    if (strcmp(flag, "no-error") == 0) {
        c->werror = false;
        return true;
    }
    if (strcmp(flag, "everything") == 0) {
        c->everything = true;
        return true;
    }
    if (strncmp(flag, "no-error=", 9) == 0) {
        for (i = 0; i < NOPTIONS; i++) {
            long lv;
            if (name_matches(&options[i], flag + 9, &lv)) {
                c->error[i] = false;
                c->noerror[i] = true;
                found = true;
            }
        }
        return found;
    }
    if (strncmp(flag, "error=", 6) == 0) {
        err = true;
        flag += 6;
    } else if (strncmp(flag, "no-", 3) == 0) {
        on = false;
        flag += 3;
    }
    /* -Wlarger-than-N is -Wlarger-than=N */
    if (!strncmp(flag, "larger-than-", 12) && isdigit((unsigned char)flag[12])) {
        snprintf(joined, sizeof joined, "larger-than=%s", flag + 12);
        flag = joined;
    }
    for (i = 0; i < NUMBRELLA; i++)
        if (strcmp(flag, umbrellas[i]) == 0) {
            /* DiagOption.by bits: all, extra, pedantic, unused, implicit */
            static const unsigned bit[NUMBRELLA] = {1, 2, 8, 16};
            size_t k;
            if (!err) {
                c->umbrella[i] = on;
                return true;
            }
            c->umbrella[i] = true;
            for (k = 0; k < NOPTIONS; k++)
                if (options[k].by & bit[i]) {
                    c->overrides[k] = options[k].level;
                    c->overridden[k] = true;
                    c->error[k] = true;
                }
            return true;
        }
    for (i = 0; i < NOPTIONS; i++) {
        long lv;
        if (name_matches(&options[i], flag, &lv) ||
            (!err && strcmp(options[i].group, flag) == 0)) {
            bool en = on && lv != 0;
            uint64_t sz = 0;
            if (!strcmp(options[i].name, "larger-than=") && on &&
                (!strchr(flag, 61) || !parse_size(strchr(flag, 61) + 1, &sz)))
                return false;       /* the driver reports the bad size */
            if (!strcmp(options[i].name, "conversion") && !err)
                c->conv = en ? 1 : -1;
            c->overrides[i] = en ? options[i].level : DL_IGNORED;
            c->overridden[i] = true;
            c->optlevel[i] = strchr(flag, 61) ? (signed char)lv : 0;
            c->optsize[i] = sz;
            if (err) {
                c->error[i] = true;
                c->noerror[i] = false;
            }
            found = true;
        }
    }
    if (found && strcmp(flag, "pedantic") == 0)
        c->pedantic = on;
    return found;
}

/* Umbrella state for option i: 1 enabled, 0 turned off, -1 unset. */
static int umbrella_state(const DiagConfig *c, const DiagOption *o)
{
    int all, extra, unused, implicit, st = -1;
    if (!c || !o->by)
        return -1;
    all = c->umbrella[U_ALL];
    extra = c->umbrella[U_EXTRA];
    unused = c->umbrella[U_UNUSED] >= 0 ? c->umbrella[U_UNUSED] : all;
    implicit = c->umbrella[U_IMPLICIT] >= 0 ? c->umbrella[U_IMPLICIT] : all;
#define UMB(bit, v)                                                         \
    if ((o->by & (bit)) && (v) >= 0) {                                      \
        if ((v) > 0)                                                        \
            return 1;                                                       \
        st = 0;                                                             \
    }
    UMB(DO_ALL, all)
    UMB(DO_EXTRA, extra)
    UMB(DO_PEDANTIC, c->pedantic ? 1 : -1)
    UMB(DO_UNUSED, unused)
    UMB(DO_IMPLICIT, implicit)
    UMB(DO_UNUSED_EXTRA, unused < 0 || extra < 0 ? -1 : unused && extra)
#undef UMB
    return st;
}

/* The option's state and level under c: *lvl is DL_IGNORED when off. */
static int option_state(const DiagConfig *c, size_t i, DiagLevel *lvl)
{
    const DiagOption *o = &options[i];
    int u;
    if (c && c->overridden[i]) {
        *lvl = c->overrides[i];
        if (*lvl != DL_IGNORED && c->error[i])
            *lvl = DL_ERROR;
        return *lvl != DL_IGNORED;
    }
    /* gcc: -Wconversion enables -Wsign-conversion and -Wfloat-conversion in C
     * unless they were set explicitly */
    if (c && c->conv && (!strcmp(o->name, "sign-conversion") ||
                         !strcmp(o->name, "float-conversion"))) {
        if (c->conv > 0) {
            *lvl = o->level;
            return 1;
        }
        *lvl = DL_IGNORED;
        return 0;
    }
    /* gcc: -Wformat=2 enables -Wformat-nonliteral and -Wformat-security */
    if (c && (!strcmp(o->name, "format-nonliteral") ||
              !strcmp(o->name, "format-security"))) {
        static __thread long f = -2;
        if (f == -2)
            f = opt_index(DW_FORMAT_V);
        if (f >= 0 && c->overridden[f] && c->overrides[f] != DL_IGNORED &&
            c->optlevel[f] >= 2) {
            *lvl = o->level;
            return 1;
        }
        if (f >= 0 && c->overridden[f] && c->overrides[f] == DL_IGNORED &&
            !strcmp(o->name, "format-security")) {      /* -Wno-format */
            *lvl = DL_IGNORED;
            return 0;
        }
    }
    /* gcc: -Wshadow enables -Wshadow=local, which enables
     * -Wshadow=compatible-local */
    if (!strcmp(o->name, "shadow=local") ||
        !strcmp(o->name, "shadow=compatible-local"))
        return option_state(c, i - 1, lvl);
    if (c && !strcmp(o->name, "dangling-else")) {       /* gcc: enabled by -Wparentheses */
        static __thread long p = -2;
        if (p == -2)
            p = opt_index(DW_PARENTHESES);
        if (p >= 0 && c->overridden[p])
            return option_state(c, (size_t)p, lvl);
    }
    /* gcc: -Wc90-c99-compat enables -Wlong-long */
    if (c && !strcmp(o->name, "long-long")) {
        static __thread long p = -2;
        if (p == -2)
            p = opt_index(DW_C90_C99_COMPAT);
        if (p >= 0 && c->overridden[p] && c->overrides[p] != DL_IGNORED) {
            *lvl = o->level;
            return 1;
        }
    }
    u = umbrella_state(c, o);
    if (u > 0 || (c && c->everything) || (u < 0 && o->on)) {
        *lvl = o->level;
        return 1;
    }
    *lvl = DL_IGNORED;
    return u == 0 ? 0 : -1;
}

static DiagLevel diag_level_for(DiagEngine *d, DiagOpt id, DiagLevel requested)
{
    long i = opt_index(id);
    DiagLevel l;
    if (i < 0 || requested >= DL_ERROR || requested == DL_NOTE)
        return requested;
    {
        uint32_t g = d->cfg ? d->cfg->gen : 0;
        _Static_assert(sizeof options / sizeof *options <= sizeof d->memo,
                       "DiagEngine.memo too small");
        if (d->memo_gen != g) {
            memset(d->memo, 0, sizeof d->memo);
            d->memo_gen = g;
        }
        if (d->memo[i])
            return (DiagLevel)(d->memo[i] - 1);
        option_state(d->cfg, (size_t)i, &l);
        d->memo[i] = (uint8_t)(l + 1);
    }
    return l;
}

static bool diag_noerror(DiagEngine *d, DiagOpt id)
{
    long i = opt_index(id);
    return d->cfg && i >= 0 && d->cfg->noerror[i];
}

bool diag_enabled(DiagEngine *d, DiagOpt id)
{
    return diag_level_for(d, id, DL_WARNING) != DL_IGNORED;
}

uint8_t *diag_enabled_map(DiagEngine *d)
{
    uint8_t *m = xmalloc(NOPTIONS);
    size_t i;
    for (i = 0; i < NOPTIONS; i++) {
        DiagLevel l = diag_level_for(d, (DiagOpt)(i + 1), DL_WARNING);
        m[i] = l != DL_IGNORED && !(l == DL_WARNING && d->no_warnings);
    }
    return m;
}

bool diag_map_enabled(const uint8_t *map, const char *id)
{
    long i;
    if (!id || !*id)
        return true;
    i = find_index(id);
    return i < 0 || map[i];
}

int diag_option_level(DiagEngine *d, DiagOpt id, int dflt)
{
    long i = opt_index(id);
    if (i < 0 || !d->cfg || !d->cfg->optlevel[i])
        return dflt;
    return d->cfg->optlevel[i];
}

/* -Wfoo=SIZE: the size as given (units applied); 0 when not given. */
uint64_t diag_option_size(DiagEngine *d, DiagOpt id)
{
    long i = opt_index(id);
    return i < 0 || !d->cfg ? 0 : d->cfg->optsize[i];
}

int diag_option_state(DiagEngine *d, DiagOpt id)
{
    long i = opt_index(id);
    DiagLevel l;
    if (i < 0)
        return 1;
    return option_state(d->cfg, (size_t)i, &l);
}

bool diag_option_explicit(DiagEngine *d, DiagOpt id)
{
    long i = opt_index(id);
    return i >= 0 && d->cfg && d->cfg->overridden[i] &&
           d->cfg->overrides[i] != DL_IGNORED;
}

/* Asked for: explicitly, or through -Wall/-Wextra (not by default). */
bool diag_option_requested(DiagEngine *d, DiagOpt id)
{
    long i = opt_index(id);
    if (i < 0 || !d->cfg)
        return false;
    return diag_option_explicit(d, id) || umbrella_state(d->cfg, &options[i]) > 0;
}

static bool in_system_header(DiagEngine *d, SrcLoc loc)
{
    SrcFile *f = srcmgr_file_of(d->sm, loc);
    return f && srcmgr_is_system(f, loc);    /* with '# N "f" 3' markers */
}

bool diag_hidden_in_system_header(DiagEngine *d, SrcLoc loc)
{
    return !d->show_system && in_system_header(d, loc);
}

Diagnostic *diag_vreport(DiagEngine *d, DiagLevel lvl, DiagOpt id,
                         SrcLoc loc, const char *fmt, va_list ap)
{
    Diagnostic *dg;
    StrBuf sb = {0};
    DiagLevel req = lvl;
    bool promoted, nocol = d->nocol_next;
    uint32_t vcol;
    d->nocol_next = false;
    vcol = d->vcol_next;
    d->vcol_next = 0;
    lvl = diag_level_for(d, id, lvl);
    if (lvl == DL_IGNORED || (lvl == DL_WARNING && d->no_warnings))
        return NULL;
    /* a warning asked for is suppressed in a system header even when
     * -pedantic-errors or -Werror made it an error */
    if ((lvl <= DL_WARNING || (lvl == DL_ERROR && id != DW_NONE &&
                               (req == DL_WARNING || d->pedantic_errors))) &&
        !d->show_system && in_system_header(d, loc))
        return NULL;
    if (lvl == DL_WARNING && d->werror && !diag_noerror(d, id))
        lvl = DL_ERROR;
    promoted = id != DW_NONE && req < DL_ERROR && req != DL_NOTE && lvl == DL_ERROR;
    dg = NEW(d->arena, Diagnostic);
    dg->promoted = promoted;
    dg->nocol = nocol;
    dg->vcol = vcol;
    dg->ord = d->ord;
    dg->level = lvl;
    dg->id = diag_opt_name(id);
    dg->loc = loc;
    dg->oloc = loc;
    dg->key = d->key;
    sb_vprintf(&sb, fmt, ap);
    dg->msg = arena_strndup(d->arena, sb_cstr(&sb), sb.len);
    sb_free(&sb);
    {
        /* the chain of the location's own file, as gcc prints it; a location
         * with no file of its own (a macro scratch spelling) takes the
         * preprocessor's current one */
        SrcLoc cl[16];
        int n = 0;
        SrcFile *f = d->sm ? srcmgr_file_of(d->sm, loc) : NULL;
        SrcLoc *locs = cl;
        if (f && f->kind != SF_SCRATCH && f->kind != SF_VIRTUAL) {
            while (f && n < 16 && __atomic_load_n(&f->inc_loc, __ATOMIC_RELAXED)) {
                cl[n] = __atomic_load_n(&f->inc_loc, __ATOMIC_RELAXED);
                f = srcmgr_file_of(d->sm, cl[n++]);
            }
        } else if (d->include_chain) {
            d->include_chain(d->include_chain_ctx, &locs, &n);
        }
        if (n > 0) {
            dg->inc_chain = NEW_ARRAY(d->arena, SrcLoc, n);
            memcpy(dg->inc_chain, locs, sizeof(SrcLoc) * (size_t)n);
            dg->ninc = n;
        }
    }
    if (lvl >= DL_ERROR)
        d->nerrors++;
    else if (lvl == DL_WARNING)
        d->nwarnings++;
    vec_push(&d->all, dg);
    return dg;
}

Diagnostic *diag_report(DiagEngine *d, DiagLevel lvl, DiagOpt id,
                        SrcLoc loc, const char *fmt, ...)
{
    Diagnostic *dg;
    va_list ap;
    va_start(ap, fmt);
    dg = diag_vreport(d, lvl, id, loc, fmt, ap);
    va_end(ap);
    return dg;
}

void diag_note(DiagEngine *d, Diagnostic *dg, SrcLoc loc, const char *fmt, ...)
{
    DiagNote n = {0};
    StrBuf sb = {0};
    va_list ap;
    if (!dg)
        return;
    va_start(ap, fmt);
    sb_vprintf(&sb, fmt, ap);
    va_end(ap);
    n.loc = loc;
    n.msg = arena_strndup(d->arena, sb_cstr(&sb), sb.len);
    sb_free(&sb);
    vec_push(&dg->notes, n);
}

void diag_note_nocol(DiagEngine *d, Diagnostic *dg, SrcLoc loc, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    if (!dg)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    diag_note(d, dg, loc, "%s", buf);
    dg->notes.data[dg->notes.len - 1].nocol = true;
}

void diag_set_range(Diagnostic *dg, SrcLoc b, SrcLoc e)
{
    if (!dg)
        return;
    dg->range.begin = b;
    dg->range.end = e;
}

static const char *level_name(DiagLevel l)
{
    switch (l) {
    case DL_NOTE: return "note";
    case DL_REMARK: return "remark";
    case DL_WARNING: return "warning";
    case DL_ERROR: return "error";
    case DL_FATAL: return "fatal error";
    default: return "?";
    }
}

static const char *level_color(DiagLevel l)
{
    switch (l) {
    case DL_NOTE: return "\033[1;36m";
    case DL_REMARK: return "\033[1;34m";
    case DL_WARNING: return "\033[1;35m";
    default: return "\033[1;31m";
    }
}

/* libcpp's cpp_wcwidth: 0 for combining marks, format characters and Hangul
 * medial/final jamo, 2 for East Asian wide, else 1. */
static int wc_width(uint32_t c)
{
    size_t lo, hi, n;
    if (c < 0x300)
        return 1;
    n = sizeof wc_zero / sizeof *wc_zero;
    for (lo = 0, hi = n; lo < hi;) {
        size_t mid = (lo + hi) / 2;
        if (c < wc_zero[mid].lo)
            hi = mid;
        else if (c > wc_zero[mid].hi)
            lo = mid + 1;
        else
            return 0;
    }
    n = sizeof wc_wide / sizeof *wc_wide;
    for (lo = 0, hi = n; lo < hi;) {
        size_t mid = (lo + hi) / 2;
        if (c < wc_wide[mid].lo)
            hi = mid;
        else if (c > wc_wide[mid].hi)
            lo = mid + 1;
        else
            return 2;
    }
    return 1;
}

/* Display width of the character at text[i] (n bytes available) and its
 * length in bytes: a tab is handled by the caller; a byte that does not
 * start a valid sequence counts as one column. */
static uint32_t char_width(const char *text, uint32_t i, uint32_t n,
                           uint32_t *nb)
{
    uint32_t cp;
    int k;
    if ((unsigned char)text[i] < 0x80 ||
        !(k = utf8_dec((const unsigned char *)text + i, n - i, &cp))) {
        *nb = 1;
        return 1;
    }
    *nb = (uint32_t)k;
    return (uint32_t)wc_width(cp);
}

/* The option libcpp names in its diagnostics: gcc tags #warning -Wcpp and
 * prints the pedwarns of libcpp's directive handling with no option. */
static bool diag_shown_id(const char **id)
{
    if (!strcmp(*id, "pp-warning-directive")) {
        *id = "cpp";
        return true;
    }
    return strcmp(*id, "include-next-in-primary") &&
           strcmp(*id, "directive-in-macro-args") &&
           strcmp(*id, "invalid-pp-token") &&
           strcmp(*id, "extra-tokens") && strcmp(*id, "integer-overflow-in-if") &&
           strcmp(*id, "sign-promo-in-if") &&
           strcmp(*id, "macro-redefined");
}

/* gcc's default column unit is the display column (cpp_byte_column_to_
 * display_column): the width of the first `col` bytes of the line, tabs
 * advancing to the next multiple of 8, characters by wcwidth, a sequence cut
 * short by col counting one column a byte, bytes past the end one each. */
static uint32_t display_col(SrcFile *f, uint32_t line, uint32_t col)
{
    uint32_t len, i = 0, dc = 0, end;
    const char *text;
    if (f->kind == SF_VIRTUAL)
        return col;
    text = srcmgr_line_text(f, line, &len);
    if (!text)
        return col;
    end = col < len ? col : len;
    while (i < end) {
        uint32_t nb;
        if (text[i] == '\t') {
            dc = (dc + 8) & ~7u;
            i++;
        } else {
            dc += char_width(text, i, end, &nb);
            i += nb;
        }
    }
    return dc + (col > len ? col - len : 0);
}

static void print_loc_line(DiagEngine *d, SrcLoc loc, DiagLevel lvl,
                           const char *msg, const char *id, SrcRange range,
                           bool dg_promoted, bool nocol, uint32_t vcol)
{
    FILE *o = d->out;
    SrcFile *f = srcmgr_file_of(d->sm, loc);
    uint32_t line = 0, col = 0, pline = 0;
    const char *fname = NULL;
    bool eof = false;
    if (d->color)
        fputs("\033[1m", o);
    if (f) {
        srcmgr_linecol(f, loc, &line, &col);
        eof = nocol || (f->kind != SF_VIRTUAL && srcmgr_offset(f, loc) == f->size);
        if (eof && !nocol && col > 1)
            line++;     /* the implied final newline */
        pline = srcmgr_presumed(f, line, &fname);
        /* gcc: a #line that is the last thing in the file has not started
         * its line map when the end of file is reached, one line further on */
        if (eof && !nocol && f->nlinemap && f->linemap[f->nlinemap - 1].from == line)
            pline++;
        if (f->kind == SF_VIRTUAL)      /* <built-in>, <command-line>: no position */
            fprintf(o, "%s: ", f->name);
        else if (!pline)        /* gcc: line 0 prints no position */
            fprintf(o, "%s: ", fname);
        else if (eof)   /* gcc: the end-of-file token has no column */
            fprintf(o, "%s:%d: ", fname, (int)pline);
        else
            fprintf(o, "%s:%d:%u: ", fname, (int)pline,
                    display_col(f, line, vcol ? vcol : col));
    } else {
        fputs("cereal: ", o);
    }
    if (d->color)
        fputs(level_color(lvl), o);
    fprintf(o, "%s: ", level_name(lvl));
    if (d->color)
        fputs("\033[0m\033[1m", o);
    fputs(msg, o);
    if (d->color)
        fputs("\033[0m", o);
    if (id && *id && diag_shown_id(&id))
        fprintf(o, dg_promoted ? " [-Werror=%s]" : " [-W%s]",
                strcmp(id, "strict-aliasing=") && strcmp(id, "cast-align=")
                    ? id : (id[0] == 99 ? "cast-align" : "strict-aliasing"));
    fputc('\n', o);
    if (nocol && f && f->kind != SF_VIRTUAL && pline) {   /* no caret line */
        uint32_t len;
        const char *text = srcmgr_line_text(f, line, &len);
        fprintf(o, "%5u | %.*s\n      | \n", pline, (int)len, text);
        return;
    }
    /* gcc reads the snippet from the presumed file at the presumed line */
    if (f && f->kind != SF_VIRTUAL && !eof && pline &&
        !strcmp(fname, f->name) &&
        (srcmgr_line_text(f, pline, &(uint32_t){0}), pline <= f->nlines)) {
        uint32_t len, i, caret_end = col;
        const char *text;
        line = pline;
        text = srcmgr_line_text(f, line, &len);
        uint32_t dcol = display_col(f, line, col), dend, dc = 0;
        fprintf(o, "%5u | ", line);
        for (i = 0; i < len; i++) {     /* gcc expands tabs to 8-column stops */
            if (text[i] == '\t') {
                do
                    fputc(' ', o);
                while (++dc & 7);
            } else {
                uint32_t nb;
                dc += char_width(text, i, len, &nb);
                fwrite(text + i, 1, nb, o);
                i += nb - 1;
            }
        }
        fputs("\n      | ", o);
        if (range.end > range.begin && srcmgr_file_of(d->sm, range.end) == f) {
            uint32_t el, ec;
            srcmgr_linecol(f, range.end, &el, &ec);
            const char *en;
            el = srcmgr_presumed(f, el, &en);
            if (el == line && ec > col)
                caret_end = ec - 1;
        }
        dend = display_col(f, line, caret_end);
        for (i = 1; i < dcol; i++)
            fputc(' ', o);
        if (d->color)
            fputs("\033[1;32m", o);
        fputc('^', o);
        for (i = dcol + 1; i <= dend && caret_end <= len; i++)
            fputc('~', o);
        if (d->color)
            fputs("\033[0m", o);
        fputc('\n', o);
    }
}

static void print_fixits(DiagEngine *d, const Diagnostic *dg, int owner);

static void diag_print(DiagEngine *d, Diagnostic *dg)
{
    int i, own = 0;
    size_t k;
    SrcRange none = {0, 0};
    for (i = dg->ninc - 1; i >= 0; i--) {
        SrcFile *f = srcmgr_file_of(d->sm, dg->inc_chain[i]);
        uint32_t line, col;
        if (!f)
            continue;
        const char *fname;
        srcmgr_linecol(f, dg->inc_chain[i], &line, &col);
        line = srcmgr_presumed(f, line, &fname);
        fprintf(d->out, "%s %s:",
                i == dg->ninc - 1 ? "In file included from" : "                 from",
                fname);
        if (f->kind == SF_VIRTUAL)      /* gcc: <command-line>: has no line */
            fputc('\n', d->out);
        else
            fprintf(d->out, "%u:\n", line);
    }
    print_loc_line(d, dg->loc, dg->level, dg->msg, dg->id, dg->range,
                   dg->promoted, dg->nocol, dg->vcol);
    /* an owner's fix-it lines follow it and its "in expansion of" notes */
    for (k = 0; k < dg->notes.len; k++) {
        if (!dg->notes.data[k].expn)
            break;
        print_loc_line(d, dg->notes.data[k].loc, DL_NOTE,
                       dg->notes.data[k].msg, NULL, none, false,
                       dg->notes.data[k].nocol, 0);
    }
    if (d->fixit_lines)
        print_fixits(d, dg, 0);
    for (; k < dg->notes.len; k++) {
        if (!dg->notes.data[k].expn)
            own = (int)k + 1;
        print_loc_line(d, dg->notes.data[k].loc, DL_NOTE,
                       dg->notes.data[k].msg, NULL, none, false,
                       dg->notes.data[k].nocol, 0);
        if (d->fixit_lines && (k + 1 == dg->notes.len ||
                               !dg->notes.data[k + 1].expn))
            print_fixits(d, dg, own);
    }
}

void diag_flush(DiagEngine *d)
{
    size_t i;
    for (i = 0; i < d->all.len; i++) {
        diag_print(d, d->all.data[i]);
        if (d->all.data[i]->level == DL_FATAL) {
            fprintf(d->out, "compilation terminated.\n");
            break;
        }
        if (d->fatal_errors && d->all.data[i]->level >= DL_ERROR) {
            fprintf(d->out, "compilation terminated due to -Wfatal-errors.\n");
            break;
        }
    }
    if (d->gen_patch)
        diag_generate_patch(d);
}

static void json_loc(JsonWriter *w, SrcMgr *sm, SrcLoc loc)
{
    SrcFile *f = srcmgr_file_of(sm, loc);
    uint32_t line = 0, col = 0;
    if (f)
        srcmgr_linecol(f, loc, &line, &col);
    json_key(w, "file");
    if (f)
        json_str(w, f->name);
    else
        json_null(w);
    json_key(w, "line");
    json_int(w, line);
    json_key(w, "col");
    json_int(w, col);
}

static const char *const app_name[] = {"safe", "maybe", "info"};

/* "fixits": [...] of one owner, omitted when it has none */
static void json_fixits(JsonWriter *w, DiagEngine *d, const Diagnostic *dg,
                        int owner)
{
    size_t k;
    bool open = false;
    for (k = 0; k < dg->fixits.len; k++) {
        const FixIt *x = &dg->fixits.data[k];
        if (x->owner != owner)
            continue;
        if (!open) {
            json_key(w, "fixits");
            json_begin_array(w);
            open = true;
        }
        json_begin_object(w);
        json_key(w, "start");
        json_begin_object(w);
        json_loc(w, d->sm, x->begin);
        json_end_object(w);
        json_key(w, "next");
        json_begin_object(w);
        json_loc(w, d->sm, x->end);
        json_end_object(w);
        json_key(w, "string");
        json_str(w, x->text);
        json_key(w, "applicability");
        json_str(w, app_name[x->app]);
        json_end_object(w);
    }
    if (open)
        json_end_array(w);
}

void diag_print_json(DiagEngine *d, FILE *out)
{
    JsonWriter w;
    size_t i, k;
    json_init(&w, out);
    json_begin_array(&w);
    for (i = 0; i < d->all.len; i++) {
        Diagnostic *dg = d->all.data[i];
        json_begin_object(&w);
        json_key(&w, "level");
        json_str(&w, level_name(dg->level));
        json_key(&w, "id");
        json_str(&w, dg->id);
        json_loc(&w, d->sm, dg->loc);
        json_key(&w, "message");
        json_str(&w, dg->msg);
        json_fixits(&w, d, dg, 0);
        json_key(&w, "notes");
        json_begin_array(&w);
        for (k = 0; k < dg->notes.len; k++) {
            json_begin_object(&w);
            json_loc(&w, d->sm, dg->notes.data[k].loc);
            json_key(&w, "message");
            json_str(&w, dg->notes.data[k].msg);
            json_fixits(&w, d, dg, (int)k + 1);
            json_end_object(&w);
        }
        json_end_array(&w);
        json_end_object(&w);
    }
    json_end_array(&w);
    fputc('\n', out);
}

/* Merges the diagnostics reported since all[from] (the parser's, up to
 * all[mid]) with the checker's (from mid on) by location, each side keeping
 * its own order: gcc checks as it parses, so a syntax error follows the
 * diagnostics of what precedes it. */
/* Does the checker diagnostic j go before the parser diagnostic i? */
static bool checker_first(const DiagEngine *d, size_t from, size_t i, size_t j)
{
    const Diagnostic *p = d->all.data[i], *c = d->all.data[j];
    if (c->ord == ORD_CUT)
        return i > from && d->all.data[i - 1]->oloc >= c->oloc;
    if (c->ord == ORD_LATE)
        return false;
    if (p->ord == ORD_EOF)
        return true;
    if (!p->oloc)
        return false;
    /* within one macro invocation, by spelled location */
    return c->oloc < p->oloc ||
           (c->oloc == p->oloc &&
            (c->loc < p->loc ||
             (c->loc == p->loc &&
              (p->ord == ORD_TIE || c->ord == ORD_EARLY))));
}

void diag_merge_from(DiagEngine *d, size_t from, size_t mid)
{
    size_t i = from, j = mid, n = d->all.len - from, k = 0;
    Diagnostic **out;
    if (mid <= from || mid >= d->all.len)
        return;
    out = xmalloc(n * sizeof *out);
    while (i < mid && j < d->all.len)
        out[k++] = checker_first(d, from, i, j) ? d->all.data[j++]
                                                : d->all.data[i++];
    while (i < mid)
        out[k++] = d->all.data[i++];
    while (j < d->all.len)
        out[k++] = d->all.data[j++];
    memcpy(d->all.data + from, out, n * sizeof *out);
    free(out);
}

void diag_expansion_notes(DiagEngine *d, Diagnostic *dg, const SrcMgr *sm,
                          SrcLoc loc, SrcLoc exp, bool body,
                          MacroChainFn chain, void *ctx)
{
    MacroNote notes[10];
    size_t k, cnt = 0;
    if (chain && body)
        cnt = chain(ctx, loc, exp, notes, 10);
    if (!cnt) {
        const char *s = srcmgr_ptr(sm, exp);
        uint32_t len = 0;
        while (isalnum((unsigned char)s[len]) || s[len] == '_')
            len++;
        if (!len)
            return;
        notes[0].name = s;
        notes[0].len = len;
        notes[0].loc = exp;
        cnt = 1;
    }
    for (k = 0; k < cnt; k++) {
        diag_note(d, dg, notes[k].loc, "in expansion of macro '%.*s'",
                  (int)notes[k].len, notes[k].name);
        dg->notes.data[dg->notes.len - 1].expn = true;
    }
}

/* ---- fix-its (docs/C_DESIGN.md section 3) ------------------------------- */

bool diag_fixit(DiagEngine *d, Diagnostic *dg, int owner, SrcLoc b, SrcLoc e,
                const char *text, FixApplic app)
{
    bool *nofix, ok;
    SrcFile *f;
    uint32_t l1 = 0, c1 = 0, l2 = 0, c2 = 0;
    size_t n = strlen(text), k;
    FixIt fx;
    if (!dg)
        return false;
    nofix = owner ? &dg->notes.data[owner - 1].nofix : &dg->nofix;
    if (*nofix)
        return false;
    f = b ? srcmgr_file_of(d->sm, b) : NULL;
    ok = f && f->kind == SF_USER && b <= e && e <= f->base + f->size &&
         !srcmgr_is_system(f, b);
    if (ok) {
        srcmgr_linecol(f, b, &l1, &c1);
        srcmgr_linecol(f, e, &l2, &c2);
        /* libcpp: one line; a newline only last, in an insertion at col 1 */
        /* past LINE_MAP_MAX_COLUMN_NUMBER libcpp has no column */
        ok = l1 == l2 && c1 <= 4096 && c2 <= 4096 &&
             (!memchr(text, '\n', n) ||
                          (b == e && c1 == 1 && text[n - 1] == '\n' &&
                           memchr(text, '\n', n) == text + n - 1));
    }
    if (!ok) {
        size_t w = 0;
        *nofix = true;
        for (k = 0; k < dg->fixits.len; k++)
            if (dg->fixits.data[k].owner != owner)
                dg->fixits.data[w++] = dg->fixits.data[k];
        dg->fixits.len = w;
        return false;
    }
    for (k = dg->fixits.len; k-- > 0;)
        if (dg->fixits.data[k].owner == owner) {
            FixIt *last = &dg->fixits.data[k];
            size_t ln = strlen(last->text);
            /* libcpp consolidates a hint that starts where the last ends */
            if (last->end == b && !(ln && last->text[ln - 1] == '\n')) {
                char *s = arena_alloc(d->arena, ln + n + 1);
                memcpy(s, last->text, ln);
                memcpy(s + ln, text, n);
                last->text = s;
                last->end = e;
                return true;
            }
            break;
        }
    fx.begin = b;
    fx.end = e;
    fx.text = text;
    fx.owner = (uint16_t)owner;
    fx.app = (uint8_t)app;
    vec_push(&dg->fixits, fx);
    return true;
}

bool diag_fix_group(SrcMgr *sm, const Diagnostic *d, DiagEditVec *out)
{
    size_t base = out->len, j, m;
    bool change = false, ok = d->fixits.len && !d->nofix;
    for (j = 0; j < d->notes.len && ok; j++)
        ok = !d->notes.data[j].nofix;
    for (j = 0; j < d->fixits.len && ok; j++) {
        const FixIt *x = &d->fixits.data[j];
        SrcFile *f = x->begin ? srcmgr_file_of(sm, x->begin) : NULL;
        DiagEdit e;
        if (!f || f->kind != SF_USER || f->id < 0) {
            ok = false;
            break;
        }
        if (x->owner) {
            for (m = 0; m < d->fixits.len; m++) {
                const FixIt *y = &d->fixits.data[m];
                if (!y->owner && x->begin <= y->end && y->begin <= x->end)
                    break;
            }
            if (m < d->fixits.len)
                continue;
        }
        e.file = f;
        e.begin = srcmgr_offset(f, x->begin);
        e.end = srcmgr_offset(f, x->end);
        e.text = x->text;
        e.owner = x->owner;
        e.app = x->app;
        if (e.end - e.begin != strlen(e.text) ||
            memcmp(f->buf + e.begin, e.text, e.end - e.begin))
            change = true;
        vec_push(out, e);
    }
    if (!ok || !change) {
        out->len = base;
        return false;
    }
    return true;
}

/* Where gcc suggests adding an #include: the line after the last #include
 * before loc in loc's file (the first user file when loc is not in one),
 * else its start. */
static SrcLoc include_loc(SrcMgr *sm, SrcLoc loc)
{
    uint32_t k, n = srcmgr_nfiles(sm), line, nl, lastinc = 0;
    SrcFile *f = srcmgr_file_of(sm, loc);
    if (!f || f->kind != SF_USER || f->system_header) {
        f = NULL;
        for (k = 0; k < n; k++) {
            SrcFile *g = srcmgr_file(sm, k);
            if (g && g->kind == SF_USER && !g->system_header) {
                f = g;
                break;
            }
        }
    }
    if (!f)
        return loc;
    srcmgr_linecol(f, f->base + f->size, &nl, &line);
    if (srcmgr_file_of(sm, loc) == f)
        srcmgr_linecol(f, loc, &nl, &line);
    for (line = 1; line < nl; line++) {
        uint32_t len, j = 0;
        const char *s = srcmgr_line_text(f, line, &len);
        while (j < len && (s[j] == ' ' || s[j] == '\t'))
            j++;
        if (j >= len || s[j] != '#')
            continue;
        j++;
        while (j < len && (s[j] == ' ' || s[j] == '\t'))
            j++;
        if (len - j >= 7 && !memcmp(s + j, "include", 7))
            lastinc = line;
    }
    return srcmgr_loc_of(f, lastinc + 1, 1);
}

SrcLoc diag_header_note(DiagEngine *d, Diagnostic *dg, SrcLoc loc,
                        const char *hdr, const char *fmt, ...)
{
    StrBuf sb = {0};
    va_list ap;
    SrcLoc at;
    size_t k;
    bool seen = false;
    if (!dg)
        return loc;
    for (k = 0; k < d->hdr_seen.len && !seen; k++)
        seen = !strcmp(d->hdr_seen.data[k], hdr);
    if (!seen)
        vec_push(&d->hdr_seen, arena_strdup(d->arena, hdr));
    at = seen ? loc : include_loc(d->sm, loc);
    va_start(ap, fmt);
    sb_vprintf(&sb, fmt, ap);
    va_end(ap);
    diag_note(d, dg, at, "%s", sb_cstr(&sb));
    dg->notes.data[dg->notes.len - 1].hdr = arena_strdup(d->arena, hdr);
    sb_free(&sb);
    if (!seen)
        diag_fixit(d, dg, (int)dg->notes.len, at, at,
                   arena_printf(d->arena, "#include %s\n", hdr), FIX_SAFE);
    return at;
}

static void fixit_escape(FILE *o, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\' || c == '"')
            fprintf(o, "\\%c", c);
        else if (c == '\t')
            fputs("\\t", o);
        else if (c == '\n')
            fputs("\\n", o);
        else if (c < 0x20 || c >= 0x7f)
            fprintf(o, "\\%03o", c);
        else
            fputc(c, o);
    }
}

/* The parseable fix-it lines of one owner. */
static void print_fixits(DiagEngine *d, const Diagnostic *dg, int owner)
{
    size_t k;
    for (k = 0; k < dg->fixits.len; k++) {
        const FixIt *x = &dg->fixits.data[k];
        SrcFile *f;
        uint32_t l1, c1, l2, c2;
        const char *name;
        if (x->owner != owner)
            continue;
        f = srcmgr_file_of(d->sm, x->begin);
        srcmgr_linecol(f, x->begin, &l1, &c1);
        srcmgr_linecol(f, x->end, &l2, &c2);
        l1 = srcmgr_presumed(f, l1, &name);
        l2 = srcmgr_presumed(f, l2, &name);
        fputs("fix-it:\"", d->out);
        fixit_escape(d->out, name, strlen(name));
        fprintf(d->out, "\":{%u:%u-%u:%u}:\"", l1, c1, l2, c2);
        fixit_escape(d->out, x->text, strlen(x->text));
        fputs("\"\n", d->out);
    }
}

/* gcc's -fdiagnostics-generate-patch: all the TU's fix-its as one patch,
 * on d->out after its diagnostics.  gcc prints none once a fix-it was
 * refused, or when a fix-it's file is a #line name (edit-context.cc). */
void diag_generate_patch(DiagEngine *d)
{
    FixSet fs;
    VEC(SrcFile *) files = {0};
    size_t i, k, j;
    bool valid = true;
    fixset_init(&fs);
    for (i = 0; i < d->all.len && valid; i++) {
        const Diagnostic *dg = d->all.data[i];
        bool any = false;
        if (dg->nofix)
            valid = false;
        for (k = 0; k < dg->notes.len; k++)
            if (dg->notes.data[k].nofix)
                valid = false;
        for (k = 0; k < dg->fixits.len && valid; k++) {
            const FixIt *x = &dg->fixits.data[k];
            SrcFile *f = srcmgr_file_of(d->sm, x->begin);
            uint32_t l, c, pl;
            const char *pname;
            uint32_t fi;
            srcmgr_linecol(f, x->begin, &l, &c);
            pl = srcmgr_presumed(f, l, &pname);
            if (pl != l || strcmp(pname, f->name)) {
                valid = false;
                break;
            }
            fi = fixset_file(&fs, f->name);
            if (fi == files.len)
                vec_push(&files, f);
            if (!any)
                fixset_group(&fs);
            any = true;
            fixset_add(&fs, fi, srcmgr_offset(f, x->begin),
                       srcmgr_offset(f, x->end), x->text);
        }
    }
    if (valid && fs.edits.len) {
        fixset_resolve(&fs);
        /* files by name, as gcc's hash_map-ordered by strcmp tree */
        for (i = 0; i < files.len; i++)
            for (j = i + 1; j < files.len; j++)
                if (strcmp(files.data[j]->name, files.data[i]->name) < 0) {
                    SrcFile *t = files.data[i];
                    files.data[i] = files.data[j];
                    files.data[j] = t;
                }
        for (i = 0; i < files.len; i++) {
            SrcFile *f = files.data[i];
            fixset_diff(&fs, fixset_file(&fs, f->name), f->name, f->buf,
                        f->size, d->out);
        }
    }
    vec_free(&files);
    fixset_free(&fs);
}
