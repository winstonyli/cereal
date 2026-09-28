/* diag.c - diagnostics engine. */
#include "diag.h"
#include "json.h"

#include <string.h>

/* Every configurable diagnostic.  Hard errors use id "" and are not listed. */
static const DiagOption options[] = {
    /* preprocessor core */
    {"pp-warning-directive", "pp", DL_WARNING, true, "#warning directive"},
    {"deprecated", "pp", DL_WARNING, true, "GCC assertions (#assert, #unassert, #pred(answer) in #if)"},
    {"undef", "cond", DL_WARNING, false, "undefined identifier evaluates to 0 in #if"},
    {"macro-redefined", "pp", DL_WARNING, true, "non-identical macro redefinition (C99 6.10.3p2)"},
    {"builtin-macro-redefined", "pp", DL_WARNING, true, "redefining or undefining a predefined macro"},
    {"unknown-pragma", "pp", DL_WARNING, false, "unrecognized #pragma"},
    {"invalid-pp-token", "pp", DL_WARNING, true, "unterminated character or string literal"},
    {"directive-in-macro-args", "pp", DL_WARNING, true, "directive inside macro arguments (C99 6.10.3p11 UB)"},
    {"extra-tokens", "pp", DL_WARNING, true, "extra tokens at end of directive"},
    {"include-next-in-primary", "pp", DL_WARNING, true, "#include_next in primary source file"},
    {"expansion-to-defined", "cond", DL_WARNING, true, "macro expansion produced 'defined' in #if (UB)"},
    {"integer-overflow-in-if", "cond", DL_WARNING, true, "signed overflow in #if expression"},
    {"pedantic", "pedantic", DL_WARNING, false, "GNU extensions and non-portable constructs"},
    {"stdc-pragma", "pp", DL_WARNING, true, "malformed STDC pragma"},
    {"unbalanced-push-pop-macro", "pp", DL_WARNING, true, "#pragma pop_macro without push"},

    /* hygiene */
    {"macro-unparenthesized-param", "hygiene", DL_WARNING, true, "parameter used as an operand without parentheses"},
    {"macro-unparenthesized-body", "hygiene", DL_WARNING, true, "expression-like body without enclosing parentheses"},
    {"macro-multi-statement", "hygiene", DL_WARNING, true, "statement-like body not wrapped in do { } while (0)"},
    {"macro-dangling-else", "hygiene", DL_WARNING, true, "body is an if-statement without else (dangling else hazard)"},
    {"macro-trailing-semicolon", "hygiene", DL_WARNING, true, "body ends with ';'"},
    {"macro-multi-eval", "hygiene", DL_WARNING, true, "argument with side effects evaluated more than once"},
    {"macro-multi-eval-call", "hygiene", DL_WARNING, false, "argument containing a call evaluated more than once"},
    {"macro-discarded-side-effect", "hygiene", DL_WARNING, true, "argument with side effects never evaluated"},
    {"macro-reserved-name", "hygiene", DL_WARNING, true, "macro name is a reserved identifier or keyword"},
    {"macro-unused-param", "hygiene", DL_REMARK, false, "parameter never used in the body"},
    {"macro-unbalanced", "hygiene", DL_REMARK, false, "unbalanced delimiters in the body"},
    {"macro-self-reference", "hygiene", DL_REMARK, false, "macro refers to itself (not re-expanded)"},

    /* conditional compilation */
    {"cond-dead-branch", "cond", DL_WARNING, true, "branch can never be taken in any configuration"},
    {"cond-redundant", "cond", DL_WARNING, true, "condition is always true given enclosing conditions"},
    {"cond-constant", "cond", DL_REMARK, false, "#if condition is a constant (e.g. #if 0)"},
    {"cond-typo", "cond", DL_WARNING, true, "tested macro is never defined but a similar name is"},
    {"cond-never-defined", "cond", DL_REMARK, false, "tested macro is never defined anywhere"},
    {"endif-label", "cond", DL_WARNING, true, "#endif/#else comment does not match the opening condition"},

    /* include / dependency */
    {"header-guard", "include", DL_WARNING, true, "include guard #ifndef and #define disagree"},
    {"missing-header-guard", "include", DL_WARNING, true, "header has no include guard or #pragma once"},
    {"guard-collision", "include", DL_WARNING, true, "two headers use the same include guard macro"},
    {"duplicate-include", "include", DL_WARNING, true, "same header included twice from one file"},
    {"include-cycle", "include", DL_REMARK, false, "include cycle"},
    {"unused-include", "include", DL_WARNING, true, "macro-only header provides nothing used"},
    {"unused-macros", "include", DL_WARNING, true, "macro defined in the main file is never used"},
};

#define NOPTIONS (sizeof options / sizeof options[0])

/* Warning configuration from -W flags: built once, then shared read-only
 * by every engine (translation units run concurrently). */
struct DiagConfig {
    DiagLevel overrides[NOPTIONS];
    bool overridden[NOPTIONS];
    bool everything;
    bool werror;
    bool pedantic;
};

DiagConfig *diag_config_new(void)
{
    return xcalloc(1, sizeof(DiagConfig));
}

void diag_config_free(DiagConfig *c)
{
    free(c);
}

bool diag_config_werror(const DiagConfig *c)
{
    return c && c->werror;
}

bool diag_config_pedantic(const DiagConfig *c)
{
    return c && c->pedantic;
}

const DiagOption *diag_find_option(const char *name)
{
    size_t i;
    for (i = 0; i < NOPTIONS; i++)
        if (strcmp(options[i].name, name) == 0)
            return &options[i];
    return NULL;
}

void diag_list_options(FILE *out)
{
    size_t i;
    for (i = 0; i < NOPTIONS; i++)
        fprintf(out, "  -W%-30s %-8s %-4s %s\n", options[i].name,
                options[i].group, options[i].on ? "on" : "off",
                options[i].help);
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

void diag_free(DiagEngine *d)
{
    size_t i;
    for (i = 0; i < d->all.len; i++)
        vec_free(&d->all.data[i]->notes);
    vec_free(&d->all);
}

bool diag_config_apply(DiagConfig *c, const char *flag)
{
    bool on = true;
    size_t i;
    bool found = false;
    if (strcmp(flag, "error") == 0) {
        c->werror = true;
        return true;
    }
    if (strcmp(flag, "everything") == 0) {
        c->everything = true;
        return true;
    }
    if (strcmp(flag, "all") == 0 || strcmp(flag, "extra") == 0) {
        /* every off-by-default warning (not remarks, not pedantic) */
        for (i = 0; i < NOPTIONS; i++)
            if (!options[i].on && options[i].level == DL_WARNING &&
                strcmp(options[i].group, "pedantic") != 0) {
                c->overrides[i] = DL_WARNING;
                c->overridden[i] = true;
            }
        return true;
    }
    if (strncmp(flag, "no-", 3) == 0) {
        on = false;
        flag += 3;
    }
    for (i = 0; i < NOPTIONS; i++) {
        if (strcmp(options[i].name, flag) == 0 ||
            strcmp(options[i].group, flag) == 0) {
            c->overrides[i] = on ? options[i].level : DL_IGNORED;
            c->overridden[i] = true;
            found = true;
        }
    }
    if (found && strcmp(flag, "pedantic") == 0)
        c->pedantic = on;
    return found;
}

DiagLevel diag_level_for(DiagEngine *d, const char *id, DiagLevel requested)
{
    size_t i;
    const DiagConfig *c = d->cfg;
    if (!id || !*id || requested >= DL_ERROR || requested == DL_NOTE)
        return requested;
    for (i = 0; i < NOPTIONS; i++) {
        if (strcmp(options[i].name, id) == 0) {
            bool ov = c && c->overridden[i];
            DiagLevel l = ov ? c->overrides[i]
                          : options[i].on ? options[i].level : DL_IGNORED;
            if (c && c->everything && !ov)
                l = options[i].level;
            if (l == DL_IGNORED)
                return DL_IGNORED;
            return l;
        }
    }
    return requested;
}

bool diag_enabled(DiagEngine *d, const char *id)
{
    return diag_level_for(d, id, DL_WARNING) != DL_IGNORED;
}

static bool in_system_header(DiagEngine *d, SrcLoc loc)
{
    SrcFile *f = srcmgr_file_of(d->sm, loc);
    return f && f->system_header;
}

Diagnostic *diag_vreport(DiagEngine *d, DiagLevel lvl, const char *id,
                         SrcLoc loc, const char *fmt, va_list ap)
{
    Diagnostic *dg;
    StrBuf sb = {0};
    lvl = diag_level_for(d, id, lvl);
    if (lvl == DL_IGNORED)
        return NULL;
    if (lvl <= DL_WARNING && !d->show_system && in_system_header(d, loc))
        return NULL;
    if (lvl == DL_WARNING && d->werror)
        lvl = DL_ERROR;
    dg = NEW(d->arena, Diagnostic);
    dg->level = lvl;
    dg->id = id ? id : "";
    dg->loc = loc;
    dg->key = d->key;
    sb_vprintf(&sb, fmt, ap);
    dg->msg = arena_strndup(d->arena, sb_cstr(&sb), sb.len);
    sb_free(&sb);
    if (d->include_chain) {
        SrcLoc *locs;
        int n;
        d->include_chain(d->include_chain_ctx, &locs, &n);
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

Diagnostic *diag_report(DiagEngine *d, DiagLevel lvl, const char *id,
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
    DiagNote n;
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

static void print_loc_line(DiagEngine *d, SrcLoc loc, DiagLevel lvl,
                           const char *msg, const char *id, SrcRange range)
{
    FILE *o = d->out;
    SrcFile *f = srcmgr_file_of(d->sm, loc);
    uint32_t line = 0, col = 0;
    if (d->color)
        fputs("\033[1m", o);
    if (f) {
        srcmgr_linecol(f, loc, &line, &col);
        fprintf(o, "%s:%u:%u: ", f->name, line, col);
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
    if (id && *id)
        fprintf(o, " [-W%s]", id);
    fputc('\n', o);
    if (f && f->kind != SF_VIRTUAL) {
        uint32_t len, i, caret_end = col;
        const char *text = srcmgr_line_text(f, line, &len);
        fprintf(o, "%5u | %.*s\n      | ", line, (int)len, text);
        if (range.end > range.begin && srcmgr_file_of(d->sm, range.end) == f) {
            uint32_t el, ec;
            srcmgr_linecol(f, range.end, &el, &ec);
            if (el == line && ec > col)
                caret_end = ec - 1;
        }
        for (i = 1; i < col && i <= len; i++)
            fputc(text[i - 1] == '\t' ? '\t' : ' ', o);
        if (d->color)
            fputs("\033[1;32m", o);
        fputc('^', o);
        for (i = col + 1; i <= caret_end && i <= len; i++)
            fputc('~', o);
        if (d->color)
            fputs("\033[0m", o);
        fputc('\n', o);
    }
}

void diag_print(DiagEngine *d, Diagnostic *dg)
{
    int i;
    size_t k;
    SrcRange none = {0, 0};
    for (i = dg->ninc - 1; i >= 0; i--) {
        SrcFile *f = srcmgr_file_of(d->sm, dg->inc_chain[i]);
        uint32_t line, col;
        if (!f)
            continue;
        srcmgr_linecol(f, dg->inc_chain[i], &line, &col);
        fprintf(d->out, "%s %s:%u:\n",
                i == dg->ninc - 1 ? "In file included from" : "                 from",
                f->name, line);
    }
    print_loc_line(d, dg->loc, dg->level, dg->msg, dg->id, dg->range);
    for (k = 0; k < dg->notes.len; k++)
        print_loc_line(d, dg->notes.data[k].loc, DL_NOTE,
                       dg->notes.data[k].msg, NULL, none);
}

void diag_flush(DiagEngine *d)
{
    size_t i;
    for (i = 0; i < d->all.len; i++)
        diag_print(d, d->all.data[i]);
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
        json_key(&w, "notes");
        json_begin_array(&w);
        for (k = 0; k < dg->notes.len; k++) {
            json_begin_object(&w);
            json_loc(&w, d->sm, dg->notes.data[k].loc);
            json_key(&w, "message");
            json_str(&w, dg->notes.data[k].msg);
            json_end_object(&w);
        }
        json_end_array(&w);
        json_end_object(&w);
    }
    json_end_array(&w);
    fputc('\n', out);
}
