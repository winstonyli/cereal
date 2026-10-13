/* indexsum.c - `cereal index-summary` (docs/B5_DESIGN.md sections 9, 14.2):
 * check one translation unit with the options the language server would
 * use, and write its manifest and records to a cache directory (--cache),
 * print them as text (--dump), or validate record files (--check). */
#include "lsp.h"

#include <string.h>
#include <unistd.h>

#include "c/cidxrec.h"
#include "hash.h"

static int check_files(int n, char **files, uint64_t build_id)
{
    int i, bad = 0;
    for (i = 0; i < n; i++) {
        size_t len;
        char *p = read_file(files[i], &len);
        int kind = p ? crec_kind(p, len) : 0;
        CRecView v;
        const char *why = !p ? "cannot read"
                         : !kind ? "not an index file"
                         : crec_parse(p, len, kind, build_id, &v);
        printf("%s: %s%s\n", files[i], why ? "bad: " : "ok ",
               why ? why : kind == CREC_RECORD ? "record" : "manifest");
        bad += why != NULL;
        free(p);
    }
    return bad ? 1 : 0;
}

int indexsum_main(int argc, char **argv)
{
    const char *root = NULL, *db = NULL, *cache = NULL, *file = NULL;
    bool dump = false;
    char cwd[4096], dbabs[8192], err[512];
    int i;
    if (argc && !strcmp(argv[0], "--check"))
        return check_files(argc - 1, argv + 1, crec_build_id());
    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc)
            root = argv[++i];
        else if (!strcmp(argv[i], "--compdb") && i + 1 < argc)
            db = argv[++i];
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc)
            cache = argv[++i];
        else if (!strcmp(argv[i], "--dump"))
            dump = true;
        else if (!file && argv[i][0] != '-')
            file = argv[i];
        else {
            fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }
    if (!file || (!dump && !cache) || !getcwd(cwd, sizeof cwd)) {
        fputs("usage: cereal index-summary [--root DIR] [--compdb PATH] "
              "(--cache DIR | --dump) FILE\n"
              "       cereal index-summary --check FILE...\n", stderr);
        return 2;
    }
    if (db && db[0] != '/') {
        snprintf(dbabs, sizeof dbabs, "%s/%s", cwd, db);
        db = dbabs;
    }
    {
        LspConfig cfg;
        Options o;
        StrVec cfiles = {0};
        CRecSet set;
        const char *abs;
        int rc = 0;
        config_init(&cfg, root ? root : cwd, db);
        abs = compdb_abs(&cfg.arena, cwd, file);
        options_init(&o);
        config_options_in(&cfg, abs, &o.rsp, &o, &cfiles);
        if (!crec_index(&o, abs, &cfiles, crec_build_id(), &set)) {
            printf("error cannot open %s\n", file);
            rc = 1;
        } else {
            if (dump)
                crec_dump(&set, cfg.root, stdout);
            if (cache) {
                char *dir = xmalloc(strlen(cache) + 64);
                uint64_t pid = hash64(cfg.root, strlen(cfg.root),
                                      hash64_str(cfg.db_path, 0));
                sprintf(dir, "%s/index/%016llx", cache, (unsigned long long)pid);
                if (crec_write(dir, &set, err, sizeof err))
                    printf("ok %016llx\n", (unsigned long long)set.tukey);
                else {
                    printf("error %s\n", err);
                    rc = 1;
                }
                free(dir);
            }
            crec_free(&set);
        }
        vec_free(&cfiles);
        options_free(&o);
        config_free(&cfg);
        return rc;
    }
}
