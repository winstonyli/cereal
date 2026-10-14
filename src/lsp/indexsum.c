/* indexsum.c - `cereal index-summary` (docs/B5_DESIGN.md sections 9, 14.2):
 * check one translation unit with the options the language server would
 * use, and write its manifest, records and chunks to a cache directory
 * (--cache), print them as text (--dump), or validate index files (--check).
 * With --entry it is the index pool's child process.  --stale lists which
 * units of a compile database the cache has fresh. */
#define _DEFAULT_SOURCE 1       /* setpriority */
#include "lsp.h"

#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "c/cidxrec.h"
#include "c/cidxstore.h"
#include "hash.h"
#include "indexjob.h"
#include "pstate.h"

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
               why ? why : kind == CREC_RECORD ? "record"
                         : kind == CREC_CHUNK ? "chunk" : "manifest");
        bad += why != NULL;
        free(p);
    }
    return bad ? 1 : 0;
}

/* Section 9: the child's own limits, first thing. */
static void limit_self(unsigned mb)
{
    (void)setpriority(PRIO_PROCESS, 0, 10);
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    {                           /* sanitizers reserve terabytes of address space */
        struct rlimit rl;          /* every SrcMgr reserves 4 GiB + 1 MiB of address space */
        unsigned long long as = (4096ull + 1 + 256 + 4ull * mb) << 20;
        rl.rlim_cur = rl.rlim_max = (rlim_t)as;
        (void)setrlimit(RLIMIT_AS, &rl);
    }
#endif
}

static const char *rel(const char *root, const char *p)
{
    size_t n = root ? strlen(root) : 0;
    return n && !strncmp(p, root, n) && p[n] == '/' ? p + n + 1 : p;
}

/* The units of the compile database and whether the cache has them fresh. */
static int stale_main(const char *root, const char *db, const char *cache, bool records)
{
    LspConfig cfg;
    Store st;
    PState ps;
    char warn[256];
    size_t i;
    int rc = 0;
    config_init(&cfg, root, db);
    if (!store_open(&st, cache, cfg.root, cfg.db_path, warn, sizeof warn)) {
        fprintf(stderr, "error: %s\n", warn);
        config_free(&cfg);
        return 1;
    }
    store_lock(&st);            /* as the scheduler would: rejected files are removed */
    pstate_init(&ps, &st, crec_build_id());
    pstate_set_config(&ps, &cfg, NULL, NULL);
    if (records)
        pstate_check_records(&ps);
    for (i = 0; i < ps.log.len; i++) {
        const char *s = strrchr(ps.log.data[i], '/');
        printf("%s\n", s ? s + 1 : ps.log.data[i]);
    }
    for (i = 0; i < ps.tus.len; i++) {
        const PTu *tu = ps.tus.data[i];
        printf("%s %s", pstate_why(tu->stale), rel(cfg.root, tu->main));
        if (tu->why)
            printf(" %s", rel(cfg.root, tu->why));
        if (tu->man && tu->man->status == CTU_OVER_LIMIT)
            printf(" (over limit)");
        printf("\n");
    }
    pstate_free(&ps);
    store_close(&st);
    config_free(&cfg);
    return rc;
}

int indexsum_main(int argc, char **argv)
{
    const char *root = NULL, *db = NULL, *cache = NULL, *file = NULL;
    bool dump = false, stale = false, records = false;
    long entry = -1;
    unsigned budget_mb = 0;
    char cwd[4096], dbabs[8192];
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
        else if (!strcmp(argv[i], "--entry") && i + 1 < argc)
            entry = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--budget-mb") && i + 1 < argc)
            budget_mb = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--dump"))
            dump = true;
        else if (!strcmp(argv[i], "--stale"))
            stale = true;
        else if (!strcmp(argv[i], "--records"))
            records = true;
        else if (!file && argv[i][0] != '-')
            file = argv[i];
        else {
            fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }
    if (!getcwd(cwd, sizeof cwd) || (stale ? !cache : (!file && entry < 0) ||
                                                 (!dump && !cache))) {
        fputs("usage: cereal index-summary [--root DIR] [--compdb PATH] "
              "(--cache DIR | --dump) FILE\n"
              "       cereal index-summary [--root DIR] [--compdb PATH] --cache DIR "
              "--entry N [--budget-mb N]\n"
              "       cereal index-summary [--root DIR] [--compdb PATH] --cache DIR "
              "--stale [--records]\n"
              "       cereal index-summary --check FILE...\n", stderr);
        return 2;
    }
    if (db && db[0] != '/') {
        snprintf(dbabs, sizeof dbabs, "%s/%s", cwd, db);
        db = dbabs;
    }
    if (stale)
        return stale_main(root ? root : cwd, db, cache, records);
    {
        LspConfig cfg;
        IdxJob job;
        IdxOut out;
        char *dir = NULL;
        uint32_t cancel = 0;
        int rc = 0;
        if (budget_mb)
            limit_self(budget_mb);
        config_init(&cfg, root ? root : cwd, db);
        if (entry >= (long)cfg.cmds.len) {
            printf("error no entry %ld\n", entry);
            config_free(&cfg);
            return 1;
        }
        if (cache)              /* a delegate of the server: no lock, no meta */
            dir = store_project_dir(cache, cfg.root, cfg.db_path);
        memset(&job, 0, sizeof job);
        job.cfg = &cfg;
        job.entry = entry;
        job.path = file ? compdb_abs(&cfg.arena, cwd, file) : NULL;
        job.dir = dir;
        job.dump = dump ? stdout : NULL;
        job.build_id = crec_build_id();
        job.max_bytes = (size_t)budget_mb << 20;
        job.cancel = &cancel;
        index_unit(&job, &out);
        if (out.result == IDX_DONE) {
            if (cache)
                printf("ok %016llx %s\n", (unsigned long long)out.tukey,
                       idx_status_name(out.tu_status));
        } else {
            printf("error %s\n", out.result == IDX_CANCELLED ? "cancelled" : out.msg);
            rc = 1;
        }
        free(dir);
        config_free(&cfg);
        return rc;
    }
}
