/* cidxstore.c - the index cache on disk (cidxstore.h, docs/B5_DESIGN.md 6). */
#define _DEFAULT_SOURCE 1       /* flock, mkdtemp, DT_* */
#include "cidxstore.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "c/cidxrec.h"
#include "hash.h"

#define GRACE_SECONDS 600       /* 6.4: a job that wrote records but no manifest yet */

uint64_t store_project_id(const char *root, const char *db, uint64_t build_id)
{
    uint64_t h = hash64(root, strlen(root), hash64_str(db, 0));
    return hash64(&build_id, sizeof build_id, h);   /* two builds never share files (6.4) */
}

char *store_project_dir(const char *cache, const char *root, const char *db,
                        uint64_t build_id)
{
    char *p = xmalloc(strlen(cache) + 48);
    sprintf(p, "%s/index/%016llx", cache,
            (unsigned long long)store_project_id(root, db ? db : "", build_id));
    return p;
}

/* ---- trees ----------------------------------------------------------------- */

static char *join(const char *a, const char *b)
{
    char *p = xmalloc(strlen(a) + strlen(b) + 2);
    sprintf(p, "%s/%s", a, b);
    return p;
}

/* Calls fn on every entry under path (symlinks are not followed), a directory's
 * entries before the directory, path itself last. */
typedef void (*WalkFn)(const char *path, const struct stat *st, void *ctx);

static void walk(const char *path, WalkFn fn, void *ctx)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL) {
            char *p;
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            p = join(path, e->d_name);
            walk(p, fn, ctx);
            free(p);
        }
        if (d)
            closedir(d);
    }
    fn(path, &st, ctx);
}

static void rm_one(const char *path, const struct stat *st, void *ctx)
{
    (void)ctx;
    (void)(S_ISDIR(st->st_mode) ? rmdir(path) : unlink(path));
}

static void store_remove_tree(const char *path)
{
    walk(path, rm_one, NULL);
}

static void add_bytes(const char *path, const struct stat *st, void *ctx)
{
    (void)path;
    if (!S_ISDIR(st->st_mode))
        *(uint64_t *)ctx += (uint64_t)st->st_size;
}

/* ---- opening --------------------------------------------------------------- */

static bool try_dir(const char *dir, char *why, size_t whyn)
{
    if (!dir_create_all(dir, why, whyn))
        return false;
    if (access(dir, W_OK | X_OK) != 0) {
        snprintf(why, whyn, "'%s' is not writable", dir);
        return false;
    }
    return true;
}

static void write_meta(const Store *s, const char *root, const char *db, uint64_t build_id)
{
    char *meta = join(s->dir, "meta"), *text = xmalloc(strlen(root) + strlen(db) + 96),
         err[256];
    struct stat st;
    if (stat(meta, &st) != 0) {
        sprintf(text, "format %u\nbuild %016llx\nroot %s\ncompdb %s\n", CREC_FORMAT,
                (unsigned long long)build_id, root, db);
        (void)file_write_atomic(meta, text, strlen(text), false, err, sizeof err);
    } else {
        (void)utimensat(AT_FDCWD, meta, NULL, 0);   /* recency for eviction (6.4) */
    }
    free(meta);
    free(text);
}

static int open_file(const char *dir, const char *name, int flags)
{
    char *p = join(dir, name);
    int fd = open(p, O_RDWR | O_CLOEXEC | flags, 0600);
    free(p);
    return fd;
}

static void close_files(Store *s)
{
    if (s->lock_fd >= 0)
        close(s->lock_fd);
    if (s->gen_fd >= 0)
        close(s->gen_fd);
    if (s->use_fd >= 0)
        close(s->use_fd);
    s->lock_fd = s->gen_fd = s->use_fd = -1;
}

/* The project directory under cache, with USE held shared (an evictor holds it
 * exclusive while it removes the directory, so a directory in use is never
 * removed; one removed while we open it is made again), LOCK and GEN open. */
static bool open_in(Store *s, const char *cache, const char *root, const char *db,
                    uint64_t build_id, char *why, size_t whyn)
{
    int tries;
    s->dir = store_project_dir(cache, root, db, build_id);
    for (tries = 0; tries < 3 && s->use_fd < 0; tries++) {
        char *use;
        struct stat a, b;
        if (!try_dir(s->dir, why, whyn))
            goto fail;
        s->use_fd = open_file(s->dir, "USE", O_CREAT);
        if (s->use_fd < 0 && errno != ENOENT)
            break;
        if (s->use_fd >= 0)
            (void)flock(s->use_fd, LOCK_SH);        /* waits out an evictor */
        use = join(s->dir, "USE");
        if (s->use_fd >= 0 && (fstat(s->use_fd, &a) != 0 || stat(use, &b) != 0 ||
                               a.st_ino != b.st_ino || a.st_dev != b.st_dev)) {
            close(s->use_fd);                       /* it was evicted: start again */
            s->use_fd = -1;
        }
        free(use);
    }
    if (s->use_fd >= 0) {
        s->lock_fd = open_file(s->dir, "LOCK", O_CREAT);
        s->gen_fd = open_file(s->dir, "GEN", O_CREAT);
    }
    if (s->use_fd < 0 || s->lock_fd < 0 || s->gen_fd < 0) {
        snprintf(why, whyn, "cannot open '%s': %s", s->dir, strerror(errno));
        goto fail;
    }
    write_meta(s, root, db, build_id);
    return true;
fail:
    close_files(s);
    free(s->dir);
    s->dir = NULL;
    return false;
}

/* Private roots are named for their owner; those of a process that is gone
 * (it crashed) are removed when the next one makes its own. */
#define PRIVATE_PREFIX "cereal-index-"

static void sweep_private(const char *tmp)
{
    DIR *d = opendir(tmp);
    struct dirent *e;
    size_t pl = strlen(PRIVATE_PREFIX);
    while (d && (e = readdir(d)) != NULL) {
        char *end, *p;
        struct stat st;
        long pid;
        if (strncmp(e->d_name, PRIVATE_PREFIX, pl))
            continue;
        pid = strtol(e->d_name + pl, &end, 10);
        if (end == e->d_name + pl || *end != '-' || pid <= 0 ||
            (kill((pid_t)pid, 0) == 0 || errno != ESRCH))
            continue;
        p = join(tmp, e->d_name);
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid())
            store_remove_tree(p);
        free(p);
    }
    if (d)
        closedir(d);
}

bool store_open(Store *s, const char *cache_dir, const char *root, const char *db,
                uint64_t build_id, char *warn, size_t warnn)
{
    char *cache = NULL, why[512];
    const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME"), *tmp;
    bool ok = false;
    memset(s, 0, sizeof *s);
    s->lock_fd = s->gen_fd = s->use_fd = -1;
    if (warn && warnn)
        warn[0] = 0;
    why[0] = 0;
    db = db ? db : "";
    if (cache_dir && !*cache_dir)
        ;                                       /* asked for memory only */
    else if (cache_dir)
        cache = xstrdup(cache_dir);
    else if (xdg && *xdg)
        cache = join(xdg, "cereal");
    else if (home && *home)
        cache = join(home, ".cache/cereal");
    if (cache) {
        ok = open_in(s, cache, root, db, build_id, why, sizeof why);
        if (ok)
            s->cache = cache;
        else
            free(cache);
    }
    if (!ok) {                                  /* 6.1: memory-only, in a private directory */
        char *templ, name[64];
        tmp = getenv("TMPDIR");
        tmp = tmp && *tmp ? tmp : "/tmp";
        sweep_private(tmp);
        snprintf(name, sizeof name, PRIVATE_PREFIX "%ld-XXXXXX", (long)getpid());
        templ = join(tmp, name);
        if (!mkdtemp(templ)) {
            free(templ);
            if (warn && warnn)
                snprintf(warn, warnn, "no index cache: %s", strerror(errno));
            return false;
        }
        s->cache = templ;
        s->private_root = true;
        if (!open_in(s, templ, root, db, build_id, why, sizeof why)) {
            if (warn && warnn)
                snprintf(warn, warnn, "no index cache: %s", why);
            store_close(s);
            return false;
        }
        if (cache_dir && *cache_dir && warn && warnn)
            snprintf(warn, warnn, "the index cache is not usable (%s); indexing is "
                     "memory-only", why);
        else if (!cache_dir && warn && warnn)
            snprintf(warn, warnn, "no index cache directory; indexing is memory-only");
    }
    return true;
}

void store_close(Store *s)
{
    close_files(s);
    if (s->private_root && s->cache)
        store_remove_tree(s->cache);
    free(s->cache);
    free(s->dir);
    memset(s, 0, sizeof *s);
    s->lock_fd = s->gen_fd = s->use_fd = -1;
}

bool store_lock(Store *s)
{
    if (!s->writer && s->lock_fd >= 0 && flock(s->lock_fd, LOCK_EX | LOCK_NB) == 0)
        s->writer = true;
    return s->writer;
}

uint64_t store_gen(Store *s)
{
    uint64_t g = 0;
    if (pread(s->gen_fd, &g, sizeof g, 0) != (ssize_t)sizeof g)
        return 0;
    return g;
}

void store_gen_bump(Store *s)
{
    uint64_t g = store_gen(s) + 1;
    if (pwrite(s->gen_fd, &g, sizeof g, 0) != (ssize_t)sizeof g) {
        /* readers only miss a change until the next one */
    }
}

/* ---- GC -------------------------------------------------------------------- */

static bool is_live(const uint64_t *v, size_t n, uint64_t k)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (v[mid] < k)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < n && v[lo] == k;
}

typedef struct Sweep {
    const char *ext;
    const uint64_t *live;
    size_t n, removed;
    time_t now;
} Sweep;

/* A file `<16 hex>.<ext>` that is not live goes once it is older than the grace
 * period; anything else is junk and goes by the same age rule. */
static void sweep_one(const char *path, const struct stat *st, void *ctx)
{
    Sweep *w = ctx;
    const char *name = strrchr(path, '/') + 1, *end;
    uint64_t id;
    bool named;
    if (S_ISDIR(st->st_mode))
        return;
    id = strtoull(name, (char **)&end, 16);
    named = end == name + 16 && *end == '.' && !strcmp(end + 1, w->ext);
    if ((!named || !is_live(w->live, w->n, id)) && w->now - st->st_mtime >= GRACE_SECONDS &&
        unlink(path) == 0)
        w->removed++;
}

static size_t sweep_sub(const char *dir, const char *sub, const char *ext,
                        const uint64_t *live, size_t n, time_t now)
{
    char *top = join(dir, sub);
    Sweep w = {ext, live, n, 0, now};
    walk(top, sweep_one, &w);
    free(top);
    return w.removed;
}

void store_gc(Store *s, const StoreLive *live, StoreGc *out)
{
    time_t now = time(NULL);
    StoreGc g;
    g.tus = sweep_sub(s->dir, "tu", "tu", live->tus, live->ntus, now);
    g.recs = sweep_sub(s->dir, "rec", "rec", live->recs, live->nrecs, now);
    g.chunks = sweep_sub(s->dir, "sys", "sys", live->chunks, live->nchunks, now);
    if (out)
        *out = g;
}

/* ---- eviction -------------------------------------------------------------- */

typedef struct Proj {
    char *dir;
    uint64_t bytes;
    time_t used;
} Proj;

static int proj_cmp(const void *pa, const void *pb)
{
    const Proj *a = pa, *b = pb;
    return a->used < b->used ? -1 : a->used > b->used;
}

/* Removes dir if no process has it open: the exclusive lock on USE is what a
 * server's shared one conflicts with, and it is held while the tree goes. */
static bool evict_dir(const char *dir)
{
    int fd = open_file(dir, "USE", 0);
    if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return false;
    }
    store_remove_tree(dir);
    if (fd >= 0)
        close(fd);
    return true;
}

size_t store_evict(Store *s, double cap_mb)
{
    char *idx = join(s->cache, "index");
    DIR *d = opendir(idx);
    struct dirent *e;
    VEC(Proj) ps = {0};
    uint64_t total = 0, cap = (uint64_t)(cap_mb * 1048576.0);
    size_t i, removed = 0;
    while (d && (e = readdir(d)) != NULL) {
        Proj p;
        char *meta;
        struct stat st;
        if (e->d_name[0] == '.')
            continue;
        p.dir = join(idx, e->d_name);
        p.bytes = 0;
        walk(p.dir, add_bytes, &p.bytes);
        meta = join(p.dir, "meta");
        p.used = stat(meta, &st) == 0 ? st.st_mtime : 0;
        free(meta);
        total += p.bytes;
        if (!strcmp(p.dir, s->dir)) {
            free(p.dir);
            continue;
        }
        vec_push(&ps, p);
    }
    if (d)
        closedir(d);
    if (ps.len > 1)
        qsort(ps.data, ps.len, sizeof *ps.data, proj_cmp);
    for (i = 0; i < ps.len; i++) {
        if (total > cap && evict_dir(ps.data[i].dir)) {
            total -= ps.data[i].bytes;
            removed++;
        }
        free(ps.data[i].dir);
    }
    vec_free(&ps);
    free(idx);
    return removed;
}
