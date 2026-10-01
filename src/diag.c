/* diag.c - diagnostics engine. */
#include "diag.h"
#include "json.h"

#include <string.h>

/* Every configurable diagnostic.  Hard errors use id "" and are not listed. */
static const DiagOption options[] = {
    /* preprocessor core */
    {"pp-warning-directive", "pp", DL_WARNING, true, 0, "#warning directive"},
    {"deprecated", "pp", DL_WARNING, true, 0, "GCC assertions (#assert, #unassert, #pred(answer) in #if)"},
    {"undef", "cond", DL_WARNING, false, 0, "undefined identifier evaluates to 0 in #if"},
    {"macro-redefined", "pp", DL_WARNING, true, 0, "non-identical macro redefinition (C99 6.10.3p2)"},
    {"builtin-macro-redefined", "pp", DL_WARNING, true, 0, "redefining or undefining a predefined macro"},
    {"unknown-pragma", "pp", DL_WARNING, false, DO_ALL | DO_EXTRA, "unrecognized #pragma"},
    {"invalid-pp-token", "pp", DL_WARNING, true, 0, "unterminated character or string literal"},
    {"directive-in-macro-args", "pp", DL_WARNING, true, 0, "directive inside macro arguments (C99 6.10.3p11 UB)"},
    {"extra-tokens", "pp", DL_WARNING, true, 0, "extra tokens at end of directive"},
    {"include-next-in-primary", "pp", DL_WARNING, true, 0, "#include_next in primary source file"},
    {"expansion-to-defined", "cond", DL_WARNING, true, 0, "macro expansion produced 'defined' in #if (UB)"},
    {"integer-overflow-in-if", "cond", DL_WARNING, true, 0, "signed overflow in #if expression"},
    {"pedantic", "pedantic", DL_WARNING, false, 0, "GNU extensions and non-portable constructs"},
    {"stdc-pragma", "pp", DL_WARNING, true, 0, "malformed STDC pragma"},
    {"unbalanced-push-pop-macro", "pp", DL_WARNING, true, 0, "#pragma pop_macro without push"},

    /* hygiene */
    {"macro-unparenthesized-param", "hygiene", DL_WARNING, true, 0, "parameter used as an operand without parentheses"},
    {"macro-unparenthesized-body", "hygiene", DL_WARNING, true, 0, "expression-like body without enclosing parentheses"},
    {"macro-multi-statement", "hygiene", DL_WARNING, true, 0, "statement-like body not wrapped in do { } while (0)"},
    {"macro-dangling-else", "hygiene", DL_WARNING, true, 0, "body is an if-statement without else (dangling else hazard)"},
    {"macro-trailing-semicolon", "hygiene", DL_WARNING, true, 0, "body ends with ';'"},
    {"macro-multi-eval", "hygiene", DL_WARNING, true, 0, "argument with side effects evaluated more than once"},
    {"macro-multi-eval-call", "hygiene", DL_WARNING, false, DO_ALL | DO_EXTRA, "argument containing a call evaluated more than once"},
    {"macro-discarded-side-effect", "hygiene", DL_WARNING, true, 0, "argument with side effects never evaluated"},
    {"macro-reserved-name", "hygiene", DL_WARNING, true, 0, "macro name is a reserved identifier or keyword"},
    {"macro-unused-param", "hygiene", DL_REMARK, false, 0, "parameter never used in the body"},
    {"macro-unbalanced", "hygiene", DL_REMARK, false, 0, "unbalanced delimiters in the body"},
    {"macro-self-reference", "hygiene", DL_REMARK, false, 0, "macro refers to itself (not re-expanded)"},
    {"macro-recursion", "hygiene", DL_REMARK, false, 0, "macros refer to each other in a cycle (not re-expanded)"},

    /* conditional compilation */
    {"cond-dead-branch", "cond", DL_WARNING, true, 0, "branch can never be taken in any configuration"},
    {"cond-redundant", "cond", DL_WARNING, true, 0, "condition is always true given enclosing conditions"},
    {"cond-constant", "cond", DL_REMARK, false, 0, "#if condition is a constant (e.g. #if 0)"},
    {"cond-typo", "cond", DL_WARNING, true, 0, "tested macro is never defined but a similar name is"},
    {"cond-never-defined", "cond", DL_REMARK, false, 0, "tested macro is never defined anywhere"},
    {"endif-label", "cond", DL_WARNING, true, 0, "#endif/#else comment does not match the opening condition"},

    /* include / dependency */
    {"header-guard", "include", DL_WARNING, true, 0, "include guard #ifndef and #define disagree"},
    {"missing-header-guard", "include", DL_WARNING, true, 0, "header has no include guard or #pragma once"},
    {"guard-collision", "include", DL_WARNING, true, 0, "two headers use the same include guard macro"},
    {"duplicate-include", "include", DL_WARNING, true, 0, "same header included twice from one file"},
    {"include-cycle", "include", DL_REMARK, false, 0, "include cycle"},
    {"unused-include", "include", DL_WARNING, true, 0, "macro-only header provides nothing used"},
    {"unused-macros", "include", DL_WARNING, true, 0, "macro defined in the main file is never used"},

    /* C: gcc 13 names and defaults under -std=c99 */
    {"implicit-int", "c", DL_WARNING, true, DO_IMPLICIT, "declaration without a type specifier defaults to int"},
    {"implicit-function-declaration", "c", DL_WARNING, true, DO_IMPLICIT, "call of an undeclared function"},
    {"multichar", "c", DL_WARNING, true, 0, "multi-character character constant"},
    {"overflow", "c", DL_WARNING, true, 0, "constant expression overflows or conversion changes a constant"},
    {"div-by-zero", "c", DL_WARNING, true, 0, "integer division by zero in a constant"},
    {"int-conversion", "c", DL_WARNING, true, 0, "implicit conversion between integer and pointer"},
    {"incompatible-pointer-types", "c", DL_WARNING, true, 0, "conversion between incompatible pointer types"},
    {"discarded-qualifiers", "c", DL_WARNING, true, 0, "conversion discards qualifiers from pointer target type"},
    {"discarded-array-qualifiers", "c", DL_WARNING, true, 0, "conversion discards qualifiers of array elements"},
    {"int-to-pointer-cast", "c", DL_WARNING, true, 0, "cast to pointer from integer of different size"},
    {"pointer-to-int-cast", "c", DL_WARNING, true, 0, "cast from pointer to integer of different size"},
    {"builtin-declaration-mismatch", "c", DL_WARNING, true, 0, "declaration of a builtin function with the wrong type"},
    {"return-local-addr", "c", DL_WARNING, true, 0, "function returns the address of a local variable"},
    {"shift-count-negative", "c", DL_WARNING, true, 0, "shift count is negative"},
    {"shift-count-overflow", "c", DL_WARNING, true, 0, "shift count >= width of type"},
    {"shift-overflow=", "c", DL_WARNING, true, 0, "left shift overflows (level 1: signed, 2: also into the sign bit)"},
    {"pointer-compare", "c", DL_WARNING, true, 0, "comparison of a pointer with a zero character constant"},
    {"switch-bool", "c", DL_WARNING, true, 0, "switch on a boolean expression"},
    {"switch-outside-range", "c", DL_WARNING, true, 0, "case value outside the range of the controlling type"},
    {"override-init-side-effects", "c", DL_WARNING, true, 0, "initialized field with side effects overwritten"},
    {"designated-init", "c", DL_WARNING, true, 0, "positional initializer for a designated_init struct"},
    {"attributes", "c", DL_WARNING, true, 0, "unknown or misplaced attribute"},
    {"ignored-attributes", "c", DL_WARNING, true, 0, "attribute ignored"},
    {"deprecated-declarations", "c", DL_WARNING, true, 0, "use of a deprecated declaration"},
    {"address-of-packed-member", "c", DL_WARNING, true, 0, "address of a packed member may be unaligned"},
    {"packed-bitfield-compat", "c", DL_WARNING, true, 0, "packed bit-field layout changed in GCC 4.4"},
    {"if-not-aligned", "c", DL_WARNING, true, 0, "warn_if_not_aligned type is under-aligned"},
    {"unused-result", "c", DL_WARNING, true, 0, "result of a warn_unused_result function ignored"},
    {"nonnull", "c", DL_WARNING, true, 0, "null passed to a nonnull parameter"},
    {"varargs", "c", DL_WARNING, true, 0, "questionable use of va_start"},
    {"sizeof-array-argument", "c", DL_WARNING, true, 0, "sizeof on an array parameter"},
    {"xor-used-as-pow", "c", DL_WARNING, true, 0, "2 ^ N or 10 ^ N probably meant as a power"},
    {"return-type", "c", DL_WARNING, false, DO_ALL, "return with(out) a value, or falling off a non-void function"},
    {"main", "c", DL_WARNING, false, DO_ALL | DO_PEDANTIC, "suspicious declaration of main"},
    {"pointer-sign", "c", DL_WARNING, false, DO_ALL | DO_PEDANTIC, "pointer targets differ in signedness"},
    {"parentheses", "c", DL_WARNING, false, DO_ALL, "parentheses suggested"},
    {"dangling-else", "c", DL_WARNING, false, DO_ALL, "ambiguous else"},
    {"switch", "c", DL_WARNING, false, DO_ALL, "enumeration value not handled in switch, or case not in enum"},
    {"char-subscripts", "c", DL_WARNING, false, DO_ALL, "array subscript has type char"},
    {"missing-braces", "c", DL_WARNING, false, DO_ALL, "missing braces around an initializer"},
    {"sequence-point", "c", DL_WARNING, false, DO_ALL, "operation on an object may be undefined"},
    {"duplicate-decl-specifier", "c", DL_WARNING, false, DO_ALL, "duplicate type qualifier"},
    {"enum-compare", "c", DL_WARNING, false, DO_ALL, "comparison between different enumeration types"},
    {"enum-int-mismatch", "c", DL_WARNING, false, DO_ALL, "redeclaration mixes an enumerated type and an integer type"},
    {"address", "c", DL_WARNING, false, DO_ALL, "suspicious use of an address (always true, string comparison)"},
    {"bool-compare", "c", DL_WARNING, false, DO_ALL, "boolean compared with a non-boolean constant"},
    {"bool-operation", "c", DL_WARNING, false, DO_ALL, "suspicious operation on a boolean"},
    {"logical-not-parentheses", "c", DL_WARNING, false, DO_ALL, "logical not on the left of a comparison"},
    {"tautological-compare", "c", DL_WARNING, false, DO_ALL, "comparison always evaluates to true or false"},
    {"misleading-indentation", "c", DL_WARNING, false, DO_ALL, "indentation does not reflect the block structure"},
    {"int-in-bool-context", "c", DL_WARNING, false, DO_ALL, "suspicious integer expression in a boolean context"},
    {"array-compare", "c", DL_WARNING, false, DO_ALL, "comparison of two arrays"},
    {"array-parameter=", "c", DL_WARNING, false, DO_ALL, "array parameter redeclared with a different bound"},
    {"vla-parameter", "c", DL_WARNING, false, DO_ALL, "VLA parameter redeclared with a different bound"},
    {"sizeof-pointer-div", "c", DL_WARNING, false, DO_ALL, "sizeof (pointer) / sizeof (element)"},
    {"sizeof-array-div", "c", DL_WARNING, false, DO_ALL, "sizeof (array) / sizeof (wrong type)"},
    {"nonnull-compare", "c", DL_WARNING, false, DO_ALL, "nonnull parameter compared with null"},
    {"restrict", "c", DL_WARNING, false, DO_ALL, "overlapping arguments to restrict parameters"},
    {"zero-length-bounds", "c", DL_WARNING, false, DO_ALL, "access of a zero-length array member"},
    {"packed-not-aligned", "c", DL_WARNING, false, DO_ALL, "packed struct member under-aligned"},
    {"volatile-register-var", "c", DL_WARNING, false, DO_ALL, "register variable declared volatile"},
    {"unused-variable", "c", DL_WARNING, false, DO_UNUSED, "unused local or static variable"},
    {"unused-function", "c", DL_WARNING, false, DO_UNUSED, "static function declared but not defined or used"},
    {"unused-label", "c", DL_WARNING, false, DO_UNUSED, "label defined but not used"},
    {"unused-value", "c", DL_WARNING, false, DO_UNUSED, "statement with no effect, value computed not used"},
    {"unused-but-set-variable", "c", DL_WARNING, false, DO_UNUSED, "variable set but not used"},
    {"unused-local-typedefs", "c", DL_WARNING, false, DO_UNUSED, "locally defined typedef not used"},
    {"unused-parameter", "c", DL_WARNING, false, DO_UNUSED_EXTRA, "unused function parameter"},
    {"unused-but-set-parameter", "c", DL_WARNING, false, DO_UNUSED_EXTRA, "parameter set but not used"},
    {"ignored-qualifiers", "c", DL_WARNING, false, DO_EXTRA, "type qualifiers ignored (e.g. on a return type)"},
    {"old-style-declaration", "c", DL_WARNING, false, DO_EXTRA, "storage class not at the beginning of a declaration"},
    {"missing-field-initializers", "c", DL_WARNING, false, DO_EXTRA, "struct initializer leaves fields uninitialized"},
    {"missing-parameter-type", "c", DL_WARNING, false, DO_EXTRA, "K&R parameter without a type"},
    {"override-init", "c", DL_WARNING, false, DO_EXTRA, "initialized field overwritten"},
    {"empty-body", "c", DL_WARNING, false, DO_EXTRA, "empty body of an if, else or do while"},
    {"sign-compare", "c", DL_WARNING, false, DO_EXTRA, "comparison between signed and unsigned"},
    {"type-limits", "c", DL_WARNING, false, DO_EXTRA, "comparison always true or false due to the range of a type"},
    {"shift-negative-value", "c", DL_WARNING, false, DO_EXTRA, "left shift of a negative value"},
    {"enum-conversion", "c", DL_WARNING, false, DO_EXTRA, "implicit conversion between enumerated types"},
    {"cast-function-type", "c", DL_WARNING, false, DO_EXTRA, "cast between incompatible function types"},
    {"pointer-arith", "c", DL_WARNING, false, DO_PEDANTIC, "sizeof (void), arithmetic on void * or function pointers"},
    {"overlength-strings", "c", DL_WARNING, false, DO_PEDANTIC, "string longer than the C99 minimum (4095)"},
    {"old-style-definition", "c", DL_WARNING, false, 0, "K&R function definition"},
    {"missing-prototypes", "c", DL_WARNING, false, 0, "global function defined without a prototype"},
    {"missing-declarations", "c", DL_WARNING, false, 0, "global function defined without a previous declaration"},
    {"strict-prototypes", "c", DL_WARNING, false, 0, "function declared without parameter types"},
    {"shadow", "c", DL_WARNING, false, 0, "declaration shadows another"},
    {"redundant-decls", "c", DL_WARNING, false, 0, "redundant redeclaration in the same scope"},
    {"nested-externs", "c", DL_WARNING, false, 0, "extern declaration inside a function"},
    {"bad-function-cast", "c", DL_WARNING, false, 0, "cast of a call to a non-matching type"},
    {"cast-qual", "c", DL_WARNING, false, 0, "cast removes a qualifier from the target type"},
    {"cast-align", "c", DL_WARNING, false, 0, "cast increases the required alignment"},
    {"conversion", "c", DL_WARNING, false, 0, "implicit conversion that may change a value"},
    {"sign-conversion", "c", DL_WARNING, false, 0, "implicit conversion that may change the sign"},
    {"float-conversion", "c", DL_WARNING, false, 0, "implicit conversion that reduces floating precision"},
    {"float-equal", "c", DL_WARNING, false, 0, "floating-point values compared for equality"},
    {"jump-misses-init", "c", DL_WARNING, false, 0, "goto or switch jumps over a variable initialization"},
    {"switch-default", "c", DL_WARNING, false, 0, "switch without a default case"},
    {"switch-enum", "c", DL_WARNING, false, 0, "enumeration value not handled in switch (even with default)"},
    {"declaration-after-statement", "c", DL_WARNING, false, 0, "declaration after a statement in a block"},
    {"vla", "c", DL_WARNING, false, 0, "variable length array"},
    {"long-long", "c", DL_WARNING, false, 0, "long long type"},
    {"unused-const-variable", "c", DL_WARNING, false, 0, "unused static const variable"},
    {"packed", "c", DL_WARNING, false, 0, "packed attribute has no effect or hurts alignment"},
    {"padded", "c", DL_WARNING, false, 0, "padding added to a struct"},
};

#define NOPTIONS (sizeof options / sizeof options[0])

/* Umbrella flags (gcc's EnabledBy): -1 unset, 0 off, 1 on. */
enum { U_ALL, U_EXTRA, U_UNUSED, U_IMPLICIT, NUMBRELLA };

/* Warning configuration from -W flags: built once, then shared read-only
 * by every engine (translation units run concurrently).  Explicit flags
 * win over umbrella flags whatever their order, as in gcc. */
struct DiagConfig {
    DiagLevel overrides[NOPTIONS];
    bool overridden[NOPTIONS];
    signed char optlevel[NOPTIONS];  /* -Wfoo=N: N, 0: not given */
    bool error[NOPTIONS];            /* -Werror=X */
    bool noerror[NOPTIONS];          /* -Wno-error=X: not promoted by -Werror */
    signed char umbrella[NUMBRELLA];
    bool everything;
    bool werror;
    bool pedantic;
};

DiagConfig *diag_config_new(void)
{
    DiagConfig *c = xcalloc(1, sizeof(DiagConfig));
    memset(c->umbrella, -1, sizeof c->umbrella);
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
    if (n && o->name[n - 1] == '=') {
        if (strncmp(o->name, flag, n - 1) != 0)
            return false;
        if (!flag[n - 1])
            return true;
        if (flag[n - 1] != '=' || !flag[n])
            return false;
        *level = strtol(flag + n, NULL, 10);
        return true;
    }
    return false;
}

static long find_index(const char *name)
{
    size_t i;
    long lv;
    for (i = 0; i < NOPTIONS; i++)
        if (name_matches(&options[i], name, &lv))
            return (long)i;
    return -1;
}

const DiagOption *diag_find_option(const char *name)
{
    long i = find_index(name);
    return i < 0 ? NULL : &options[i];
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

void diag_free(DiagEngine *d)
{
    size_t i;
    for (i = 0; i < d->all.len; i++)
        vec_free(&d->all.data[i]->notes);
    vec_free(&d->all);
}

bool diag_config_apply(DiagConfig *c, const char *flag)
{
    static const char *const umbrellas[NUMBRELLA] = {"all", "extra", "unused",
                                                     "implicit"};
    bool on = true, err = false;
    size_t i;
    bool found = false;
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
            c->overrides[i] = en ? options[i].level : DL_IGNORED;
            c->overridden[i] = true;
            c->optlevel[i] = strchr(flag, 61) ? (signed char)lv : 0;
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
    u = umbrella_state(c, o);
    if (u > 0 || (c && c->everything) || (u < 0 && o->on)) {
        *lvl = o->level;
        return 1;
    }
    *lvl = DL_IGNORED;
    return u == 0 ? 0 : -1;
}

static long find_index_cached(DiagEngine *d, const char *id)
{
    size_t h = ((uintptr_t)id >> 3) * 0x9E3779B1u >> 7 & 127;
    long i;
    if (d->idc_key[h] == id && !strcmp(options[d->idc_val[h]].name, id))
        return d->idc_val[h];
    i = find_index(id);
    if (i >= 0 && i < 32767 && !strcmp(options[i].name, id)) {
        d->idc_key[h] = id;
        d->idc_val[h] = (int16_t)i;
    }
    return i;
}

DiagLevel diag_level_for(DiagEngine *d, const char *id, DiagLevel requested)
{
    long i;
    DiagLevel l;
    if (!id || !*id || requested >= DL_ERROR || requested == DL_NOTE)
        return requested;
    i = find_index_cached(d, id);
    if (i < 0)
        return requested;
    option_state(d->cfg, (size_t)i, &l);
    return l;
}

bool diag_noerror(DiagEngine *d, const char *id)
{
    long i;
    if (!d->cfg || !id || !*id)
        return false;
    i = find_index_cached(d, id);
    return i >= 0 && d->cfg->noerror[i];
}

bool diag_enabled(DiagEngine *d, const char *id)
{
    return diag_level_for(d, id, DL_WARNING) != DL_IGNORED;
}

int diag_option_level(DiagEngine *d, const char *id, int dflt)
{
    long i = find_index_cached(d, id);
    if (i < 0 || !d->cfg || !d->cfg->optlevel[i])
        return dflt;
    return d->cfg->optlevel[i];
}

int diag_option_state(DiagEngine *d, const char *id)
{
    long i = find_index_cached(d, id);
    DiagLevel l;
    if (i < 0)
        return 1;
    return option_state(d->cfg, (size_t)i, &l);
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
    DiagLevel req = lvl;
    bool promoted;
    lvl = diag_level_for(d, id, lvl);
    if (lvl == DL_IGNORED)
        return NULL;
    if (lvl <= DL_WARNING && !d->show_system && in_system_header(d, loc))
        return NULL;
    if (lvl == DL_WARNING && d->werror && !diag_noerror(d, id))
        lvl = DL_ERROR;
    promoted = id && *id && req < DL_ERROR && req != DL_NOTE && lvl == DL_ERROR;
    dg = NEW(d->arena, Diagnostic);
    dg->promoted = promoted;
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

/* gcc's default column unit is the display column: tabs advance to the next
 * multiple of 8 and a UTF-8 sequence counts once. */
static uint32_t display_col(SrcFile *f, uint32_t line, uint32_t col)
{
    uint32_t len, i, dc = 0;
    const char *text;
    if (f->kind == SF_VIRTUAL)
        return col;
    text = srcmgr_line_text(f, line, &len);
    if (!text)
        return col;
    for (i = 0; i + 1 < col && i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '\t')
            dc = (dc + 8) & ~7u;
        else if ((ch & 0xC0) != 0x80)
            dc++;
    }
    return dc + 1 + (col > len + 1 ? col - len - 1 : 0);
}

static void print_loc_line(DiagEngine *d, SrcLoc loc, DiagLevel lvl,
                           const char *msg, const char *id, SrcRange range,
                           bool dg_promoted)
{
    FILE *o = d->out;
    SrcFile *f = srcmgr_file_of(d->sm, loc);
    uint32_t line = 0, col = 0;
    bool eof = false;
    if (d->color)
        fputs("\033[1m", o);
    if (f) {
        srcmgr_linecol(f, loc, &line, &col);
        eof = f->kind != SF_VIRTUAL && srcmgr_offset(f, loc) == f->size;
        if (eof && col > 1)
            line++;     /* the implied final newline */
        if (f->kind == SF_VIRTUAL && !strcmp(f->name, "<built-in>"))
            fprintf(o, "%s: ", f->name);
        else if (eof)   /* gcc: the end-of-file token has no column */
            fprintf(o, "%s:%u: ", f->name, line);
        else
            fprintf(o, "%s:%u:%u: ", f->name, line, display_col(f, line, col));
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
        fprintf(o, dg_promoted ? " [-Werror=%s]" : " [-W%s]", id);
    fputc('\n', o);
    if (f && f->kind != SF_VIRTUAL && !eof) {
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
    print_loc_line(d, dg->loc, dg->level, dg->msg, dg->id, dg->range,
                   dg->promoted);
    for (k = 0; k < dg->notes.len; k++)
        print_loc_line(d, dg->notes.data[k].loc, DL_NOTE,
                       dg->notes.data[k].msg, NULL, none, false);
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
