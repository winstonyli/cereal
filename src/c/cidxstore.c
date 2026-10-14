/* cidxstore.c - the index cache on disk (cidxstore.h, docs/B5_DESIGN.md 6). */
#define _DEFAULT_SOURCE 1       /* flock, mkdtemp, DT_* */
#include "cidxstore.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "c/cidxrec.h"
#include "hash.h"

#define GRACE_SECONDS 600       /* 6.4: a job that wrote records but no manifest yet */

uint64_t store_project_id(const char *root, const char *db)
{
    return hash64(root, strlen(root), hash64_str(db, 0));
}

char *store_project_dir(const char *cache, const char *root, const char *db)
{
    char *p = xmalloc(strlen(cache) + 48);
    sprintf(p, "%s/index/%016llx", cache,
            (unsigned long long)store_project_id(root, db ? db : ""));
    return p;
}

/* ---- trees ----------------------------------------------------------------- */

static char *join(const char *a, const char *b)
{
    char *p = xmalloc(strlen(a) + strlen(b) + 2);
    sprintf(p, "%s/%s", a, b);
    return p;
}

void store_remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        if (d) {
            while ((e = readdir(d)) != NULL) {
                char *p;
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                    continue;
                p = join(path, e->d_name);
                store_remove_tree(p);
                free(p);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

uint64_t store_bytes(const char *path)
{
    struct stat st;
    uint64_t n = 0;
    if (lstat(path, &st) != 0)
        return 0;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        if (d) {
            while ((e = readdir(d)) != NULL) {
                char *p;
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                    continue;
                p = join(path, e->d_name);
                n += store_bytes(p);
                free(p);
            }
            closedir(d);
        }
        return n;
    }
    return (uint64_t)st.st_size;
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

static void write_meta(const Store *s, const char *root, const char *db)
{
    char *meta = join(s->dir, "meta"), *text = xmalloc(strlen(root) + strlen(db) + 64),
         err[256];
    struct stat st;
    if (stat(meta, &st) != 0) {
        sprintf(text, "format %u\nroot %s\ncompdb %s\n", CREC_FORMAT, root, db);
        (void)file_write_atomic(meta, text, strlen(text), err, sizeof err);
    } else {
        (void)utimensat(AT_FDCWD, meta, NULL, 0);   /* recency for eviction (6.4) */
    }
    free(meta);
    free(text);
}

bool store_open(Store *s, const char *cache_dir, const char *root, const char *db,
                char *warn, size_t warnn)
{
    char *cache = NULL, why[512];
    const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME"), *tmp;
    bool ok = false;
    memset(s, 0, sizeof *s);
    s->lock_fd = s->gen_fd = -1;
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
        s->dir = store_project_dir(cache, root, db);
        ok = try_dir(s->dir, why, sizeof why);
        if (ok)
            s->cache = cache;
        else {
            free(cache);
            free(s->dir);
            s->dir = NULL;
        }
    }
    if (!ok) {                                  /* 6.1: memory-only, in a private directory */
        char *templ;
        tmp = getenv("TMPDIR");
        templ = join(tmp && *tmp ? tmp : "/tmp", "cereal-index-XXXXXX");
        if (!mkdtemp(templ)) {
            free(templ);
            if (warn && warnn)
                snprintf(warn, warnn, "no index cache: %s", strerror(errno));
            return false;
        }
        s->cache = templ;
        s->private_root = true;
        s->dir = store_project_dir(templ, root, db);
        if (!try_dir(s->dir, why, sizeof why)) {
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
    write_meta(s, root, db);
    {
        char *lock = join(s->dir, "LOCK"), *gen = join(s->dir, "GEN");
        s->lock_fd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        s->gen_fd = open(gen, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        free(lock);
        free(gen);
    }
    if (s->lock_fd < 0 || s->gen_fd < 0) {
        if (warn && warnn)
            snprintf(warn, warnn, "no index cache: %s", strerror(errno));
        store_close(s);
        return false;
    }
    return true;
}

void store_close(Store *s)
{
    if (s->lock_fd >= 0)
        close(s->lock_fd);
    if (s->gen_fd >= 0)
        close(s->gen_fd);
    if (s->private_root && s->cache)
        store_remove_tree(s->cache);
    free(s->cache);
    free(s->dir);
    memset(s, 0, sizeof *s);
    s->lock_fd = s->gen_fd = -1;
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

/* `<dir>/<sub>/<hh>/<16 hex>.<ext>`: files not in live, and (grace) not newer
 * than the grace period; anything else in there that is old is junk. */
static size_t sweep_sub(const char *dir, const char *sub, const char *ext,
                        const uint64_t *live, size_t n, bool grace, time_t now)
{
    char *top = join(dir, sub);
    DIR *td = opendir(top);
    struct dirent *he;
    size_t removed = 0, extn = strlen(ext);
    while (td && (he = readdir(td)) != NULL) {
        char *hd;
        DIR *d;
        struct dirent *e;
        if (he->d_name[0] == '.')
            continue;
        hd = join(top, he->d_name);
        d = opendir(hd);
        while (d && (e = readdir(d)) != NULL) {
            char *p;
            struct stat st;
            size_t len = strlen(e->d_name);
            char *end;
            uint64_t id;
            bool named;
            if (e->d_name[0] == '.' && (len == 1 || (len == 2 && e->d_name[1] == '.')))
                continue;
            p = join(hd, e->d_name);
            id = strtoull(e->d_name, &end, 16);
            named = len == 16 + 1 + extn && end == e->d_name + 16 && *end == '.' &&
                    !strcmp(end + 1, ext);
            if (lstat(p, &st) == 0 && (!named || !is_live(live, n, id)) &&
                (!(grace || !named) || now - st.st_mtime >= GRACE_SECONDS) &&
                unlink(p) == 0)
                removed++;
            free(p);
        }
        if (d)
            closedir(d);
        free(hd);
    }
    if (td)
        closedir(td);
    free(top);
    return removed;
}

void store_gc(Store *s, const StoreLive *live, StoreGc *out)
{
    time_t now = time(NULL);
    StoreGc g;
    g.tus = sweep_sub(s->dir, "tu", "tu", live->tus, live->ntus, false, now);
    g.recs = sweep_sub(s->dir, "rec", "rec", live->recs, live->nrecs, true, now);
    g.chunks = sweep_sub(s->dir, "sys", "sys", live->chunks, live->nchunks, true, now);
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

static bool dir_locked(const char *dir)
{
    char *lock = join(dir, "LOCK");
    int fd = open(lock, O_RDWR | O_CLOEXEC);
    bool held = false;
    free(lock);
    if (fd < 0)
        return false;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
        held = true;
    close(fd);                          /* drops the probe's lock */
    return held;
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
        p.bytes = store_bytes(p.dir);
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
        if (total > cap && !dir_locked(ps.data[i].dir)) {
            store_remove_tree(ps.data[i].dir);
            total -= ps.data[i].bytes;
            removed++;
        }
        free(ps.data[i].dir);
    }
    vec_free(&ps);
    free(idx);
    return removed;
}
