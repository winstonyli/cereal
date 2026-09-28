/* main.c - the `cereal` command. */
#include "driver.h"
#include "ppout.h"
#include "analysis/analysis.h"
#include "index.h"

#include <string.h>

static void usage(FILE *o)
{
    fputs(
        "usage: cereal <mode> [options] <file.c>...\n"
        "\n"
        "modes:\n"
        "  -E            preprocess to stdout (or -o FILE)\n"
        "  lint          run the preprocessor analyses\n"
        "  index         dump the macro index (LSP model) as JSON\n"
        "  query KIND FILE:LINE:COL   KIND = def | refs | hover | visible | expand\n"
        "  --list-warnings  list every -W option\n"
        "\n"
        "options:\n"
        "  -I DIR  -iquote DIR  -isystem DIR  -nostdinc\n"
        "  -D NAME[=VAL]  -U NAME  -include FILE  -undef\n"
        "  -std=c99  -pedantic  -pedantic-errors  -trigraphs\n"
        "  -W<name>  -Wno-<name>  -W<group>  -Wall  -Werror  -Weverything\n"
        "  -fdiagnostics-format=json  -fcolor-diagnostics  -P  -o FILE\n",
        o);
}

static int finish(TU *tu)
{
    int rc;
    if (tu->opt->json)
        diag_print_json(&tu->diag, stdout);
    else
        diag_flush(&tu->diag);
    rc = tu->diag.nerrors ? 1 : 0;
    return rc;
}

static int mode_preprocess(Options *o)
{
    TU tu;
    FILE *out = stdout;
    int rc;
    if (o->inputs.len != 1) {
        fputs("cereal: -E needs exactly one input file\n", stderr);
        return 2;
    }
    if (o->output && !(out = fopen(o->output, "w")))
        fatal("cannot open '%s' for writing", o->output);
    tu_init(&tu, o);
    if (tu_begin(&tu, o->inputs.data[0]))
        pp_write_output(&tu.pp, out, o->linemarkers);
    if (out != stdout)
        fclose(out);
    rc = finish(&tu);
    tu_free(&tu);
    return rc;
}

static int mode_lint(Options *o)
{
    size_t i;
    int rc = 0;
    if (o->inputs.len == 0) {
        fputs("cereal: lint needs input files\n", stderr);
        return 2;
    }
    for (i = 0; i < o->inputs.len; i++) {
        TU tu;
        Analysis an;
        tu_init(&tu, o);
        memset(&an, 0, sizeof an);
        analysis_attach(&an, &tu.pp);
        if (tu_begin(&tu, o->inputs.data[i])) {
            tu_drain(&tu);
            analysis_finish(&an);
        }
        rc |= finish(&tu);
        tu_free(&tu);
    }
    return rc;
}

static const char *loc_str(TU *tu, SrcLoc loc)
{
    SrcFile *f = srcmgr_file_of(&tu->sm, loc);
    uint32_t line = 0, col = 0;
    if (!f)
        return "<unknown>";
    srcmgr_linecol(f, loc, &line, &col);
    return arena_printf(&tu->arena, "%s:%u:%u", f->name, line, col);
}

static int mode_index(Options *o, bool all)
{
    TU tu;
    Index ix;
    int rc;
    if (o->inputs.len != 1) {
        fputs("cereal: index needs exactly one input file\n", stderr);
        return 2;
    }
    tu_init(&tu, o);
    index_init(&ix, &tu.pp);
    if (tu_begin(&tu, o->inputs.data[0])) {
        index_run(&ix);
        index_dump_json(&ix, stdout, all);
    }
    rc = tu.diag.nerrors ? 1 : 0;
    if (tu.diag.nerrors)
        diag_flush(&tu.diag);
    index_free(&ix);
    tu_free(&tu);
    return rc;
}

static const char *ref_flags(const IdxRef *r)
{
    static char buf[64];
    buf[0] = 0;
    if (r->flags & IREF_STATIC)
        strcat(buf, " body-text");
    else if (r->flags & IREF_IN_BODY)
        strcat(buf, " in-body");
    if (r->flags & IREF_FROM_ARG)
        strcat(buf, " from-arg");
    if (r->flags & IREF_PASTED)
        strcat(buf, " pasted(not-renamable)");
    if (r->flags & IREF_SYSTEM)
        strcat(buf, " system");
    return buf;
}

static void print_exp_tree(TU *tu, Index *ix, IdxExp *root)
{
    size_t i;
    int k;
    for (i = 0; i < ix->exps.len; i++) {
        IdxExp *x = ix->exps.data[i];
        if (!x || x->root != root->e)
            continue;
        printf("%*s%s", x->depth * 2, "", x->e->macro->name->str);
        if (x->e->macro->funclike) {
            putchar('(');
            for (k = 0; k < x->nargs; k++)
                printf("%s%s", k ? ", " : "", x->args[k]);
            putchar(')');
        }
        printf("  [%s]\n", loc_str(tu, x->e->name_loc));
    }
    printf("=> %s\n", sb_cstr(&root->text));
}

static int mode_query(Options *o, const char *kind, const char *at)
{
    TU tu;
    Index ix;
    int rc = 0;
    char *file, *p1, *p2;
    unsigned line, col;
    SrcFile *f;
    SrcLoc loc;
    IdxTarget t;
    int k;

    if (o->inputs.len != 1 || !at) {
        fputs("cereal: usage: cereal query KIND FILE:LINE:COL main.c\n", stderr);
        return 2;
    }
    file = xstrdup(at);
    p2 = strrchr(file, ':');
    if (!p2) {
        free(file);
        fputs("cereal: position must be FILE:LINE:COL\n", stderr);
        return 2;
    }
    *p2 = 0;
    p1 = strrchr(file, ':');
    if (!p1) {
        free(file);
        fputs("cereal: position must be FILE:LINE:COL\n", stderr);
        return 2;
    }
    *p1 = 0;
    line = (unsigned)atoi(p1 + 1);
    col = (unsigned)atoi(p2 + 1);

    tu_init(&tu, o);
    index_init(&ix, &tu.pp);
    if (!tu_begin(&tu, o->inputs.data[0])) {
        diag_flush(&tu.diag);
        free(file);
        return 1;
    }
    index_run(&ix);
    f = index_find_file(&ix, file);
    if (!f) {
        fprintf(stderr, "cereal: '%s' is not part of this translation unit\n",
                file);
        rc = 1;
        goto out;
    }
    loc = srcmgr_loc_of(f, line, col);
    t = index_resolve(&ix, loc);

    if (!strcmp(kind, "def")) {
        if (t.kind == TGT_INCLUDE)
            printf("%s:1:1\n", t.file->name);
        else if (t.kind == TGT_PARAM)
            printf("%s param %s of %s\n",
                   loc_str(&tu, t.macros[0]->param_locs[t.param]),
                   t.name->str, t.macros[0]->name->str);
        else
            for (k = 0; k < t.nmacros; k++)
                printf("%s %s\n", loc_str(&tu, t.macros[k]->name_loc),
                       macro_signature(&tu.arena, t.macros[k]));
    } else if (!strcmp(kind, "refs")) {
        IdxRef *refs;
        size_t n = index_references(&ix, &t, &refs), i;
        for (i = 0; i < n; i++)
            printf("%s %s%s\n", loc_str(&tu, refs[i].loc),
                   i < (size_t)(t.kind == TGT_PARAM ? 1 : t.nmacros)
                       ? "definition"
                       : refs[i].kind == REF_EXPANSION ? "expansion"
                       : refs[i].kind == REF_IFDEF ? "ifdef"
                       : refs[i].kind == REF_DEFINED ? "defined"
                       : refs[i].kind == REF_UNDEF ? "undef"
                       : refs[i].kind == REF_IF_VALUE ? "if-value" : "pragma",
                   ref_flags(&refs[i]));
    } else if (!strcmp(kind, "hover")) {
        if (t.kind == TGT_PARAM) {
            printf("parameter %s of %s\n", t.name->str,
                   macro_signature(&tu.arena, t.macros[0]));
        } else if (t.kind == TGT_INCLUDE) {
            printf("%s\n", t.file->path);
        } else {
            for (k = 0; k < t.nmacros; k++) {
                Macro *m = t.macros[k];
                printf("#define %s %s\n", macro_signature(&tu.arena, m),
                       macro_body_str(&tu.arena, m));
                printf("  defined at %s", loc_str(&tu, m->name_loc));
                if (m->undef_loc)
                    printf(", undefined at %s", loc_str(&tu, m->undef_loc));
                putchar('\n');
            }
            if (t.kind == TGT_MACRO && !t.nmacros)
                printf("%s: not defined\n", t.name->str);
            if (t.top)
                printf("expands to: %s\n", sb_cstr(&t.top->text));
        }
    } else if (!strcmp(kind, "visible")) {
        Macro **v;
        size_t n = index_visible(&ix, loc, &v), i;
        for (i = 0; i < n; i++)
            if (!v[i]->predefined)
                printf("%s\n", macro_signature(&tu.arena, v[i]));
    } else if (!strcmp(kind, "expand")) {
        if (t.top)
            print_exp_tree(&tu, &ix, t.top);
        else
            puts("no macro invocation here");
    } else {
        fprintf(stderr, "cereal: unknown query '%s'\n", kind);
        rc = 2;
    }
out:
    free(file);
    index_free(&ix);
    tu_free(&tu);
    return rc;
}

int main(int argc, char **argv)
{
    Options o;
    const char *mode = NULL, *qkind = NULL, *qat = NULL;
    bool all = false;
    int i, rc = 0;
    options_init(&o);
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    for (i = 1; i < argc; i++) {
        int n;
        if (!mode && (!strcmp(argv[i], "-E") || !strcmp(argv[i], "lint") ||
                      !strcmp(argv[i], "index"))) {
            mode = argv[i];
            continue;
        }
        if (!mode && !strcmp(argv[i], "query") && i + 2 < argc) {
            mode = argv[i];
            qkind = argv[++i];
            qat = argv[++i];
            continue;
        }
        if (!strcmp(argv[i], "--all")) {
            all = true;
            continue;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(stdout);
            return 0;
        }
        if (!strcmp(argv[i], "--list-warnings")) {
            diag_list_options(stdout);
            return 0;
        }
        n = options_parse_one(&o, argc, argv, i);
        if (n <= 0) {
            fprintf(stderr, "cereal: unknown option '%s'\n", argv[i]);
            return 2;
        }
        i += n - 1;
    }
    options_finish(&o);
    if (!mode) {
        usage(stderr);
        return 2;
    }
    if (!strcmp(mode, "-E"))
        rc = mode_preprocess(&o);
    else if (!strcmp(mode, "lint"))
        rc = mode_lint(&o);
    else if (!strcmp(mode, "index"))
        rc = mode_index(&o, all);
    else if (!strcmp(mode, "query"))
        rc = mode_query(&o, qkind, qat);
    options_free(&o);
    return rc;
}
