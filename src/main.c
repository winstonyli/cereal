/* main.c - the `cereal` command. */
#include "driver.h"
#include "ppout.h"
#include "analysis/analysis.h"

#include <string.h>

static void usage(FILE *o)
{
    fputs(
        "usage: cereal <mode> [options] <file.c>...\n"
        "\n"
        "modes:\n"
        "  -E            preprocess to stdout (or -o FILE)\n"
        "  lint          run the preprocessor analyses\n"
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

int main(int argc, char **argv)
{
    Options o;
    const char *mode = NULL;
    int i, rc = 0;
    options_init(&o);
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    for (i = 1; i < argc; i++) {
        int n;
        if (!mode && (!strcmp(argv[i], "-E") || !strcmp(argv[i], "lint"))) {
            mode = argv[i];
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
    options_free(&o);
    return rc;
}
