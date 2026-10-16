/* project.c - the project index scheduler (project.h, docs/B5_DESIGN.md 8, 9, 13).
 *
 * One scheduler thread owns the project state.  Everything the rest of the
 * server does reaches it as an event in a mailbox, and everything the pool
 * does comes back as a finished job; the thread keeps the store's manifests
 * fresh by running one job per stale unit.  A job is index_unit() (indexjob.h),
 * run by a pool thread in this process or by a child process
 * (`cereal index-summary --entry N`): both write the store, and the scheduler
 * publishes either by reloading the unit's manifest. */
#define _DEFAULT_SOURCE 1
#include "project.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "c/cidxrec.h"
#include "c/cidxstore.h"
#include "indexjob.h"
#include "pstate.h"
#include "projidx.h"

extern char **environ;

#define CREATE_ID "cereal-index-create"      /* the progress request we send */
#define TOKEN "cereal-index"
#define PROGRESS_MS 500                      /* at most 2 reports a second */
#define BUILDER_POLL_MS 25
#define GEN_POLL_MS 1000
#define HINT_SWEEP_MIN_MS 5000          /* between two sweeps a query asks for */
#define GC_EVERY 100                    /* manifest writes between two GCs (6.4) */
#define EVICT_MS 600000                 /* the cache is trimmed at most this often */

/* ---- settings (13.2) ----------------------------------------------------------- */

typedef struct Settings {
    bool enable;
    int threads;
    double memory_mb, max_file_mb, cache_max_mb, sweep_s, lock_retry_s;
    size_t inproc_bytes;
    unsigned quiet_ms, sub_s;
    size_t rename_max;              /* R5: the most units a rename may check */
    char *cache_dir;                /* NULL: the default (6.1) */
    VEC(char *) paths;              /* fnmatch globs, `!` excludes */
} Settings;

static const char *env_setting(const char *key)
{
    char name[80];
    size_t i, n = strlen("CEREAL_INDEX_");
    const char *e;
    memcpy(name, "CEREAL_INDEX_", n);
    for (i = 0; key[i] && n + 1 < sizeof name; i++)
        name[n++] = (char)toupper((unsigned char)key[i]);
    name[n] = 0;
    e = getenv(name);
    return e && *e ? e : NULL;
}

static double num_setting(const JsonValue *o, const char *key, double dflt)
{
    const char *e = env_setting(key);
    const JsonValue *v = o ? json_get(o, key) : NULL;
    if (e)
        return strtod(e, NULL);
    return v && v->kind == JV_NUM ? v->num : dflt;
}

static bool bool_setting(const JsonValue *o, const char *key, bool dflt)
{
    const char *e = env_setting(key);
    if (e)
        return strcmp(e, "0") && strcmp(e, "false") && strcmp(e, "off");
    return json_bool_of(o ? json_get(o, key) : NULL, dflt);
}

static void load_settings(Settings *s, const JsonValue *params, const LspConfig *cfg)
{
    const JsonValue *o = json_path(params, "initializationOptions.index"), *v;
    const char *e;
    int cores = cpu_count() / 4;
    memset(s, 0, sizeof *s);
    s->enable = bool_setting(o, "enable", cfg->db_path != NULL && cfg->root != NULL);
    s->threads = (int)num_setting(o, "threads", cores < 1 ? 1 : cores > 4 ? 4 : cores);
    if (s->threads < 1)
        s->threads = 1;
    if (s->threads > 16)
        s->threads = 16;
    s->memory_mb = num_setting(o, "memoryMB", 1536);
    s->inproc_bytes = (size_t)(num_setting(o, "maxInProcessKB",
                                           (double)(lsp_check_limit() / 1024)) * 1024);
    s->max_file_mb = num_setting(o, "maxFileMB", 64);
    if (s->max_file_mb < 1)
        s->max_file_mb = 1;
    s->cache_max_mb = num_setting(o, "cacheMaxMB", 2048);
    s->sweep_s = num_setting(o, "sweepSeconds", 30);
    s->lock_retry_s = num_setting(o, "lockRetrySeconds", 30);
    s->quiet_ms = (unsigned)num_setting(o, "quietMs", 500);
    s->sub_s = (unsigned)num_setting(o, "subprocessSeconds", 120);
    s->rename_max = (size_t)num_setting(o, "renameMaxTUs", 100);
    if ((e = env_setting("cacheDir")) != NULL)
        s->cache_dir = xstrdup(e);
    else if ((v = o ? json_get(o, "cacheDir") : NULL) != NULL && v->kind == JV_STR)
        s->cache_dir = xstrdup(v->str);
    if ((e = env_setting("paths")) != NULL) {
        char *copy = xstrdup(e), *save = NULL, *t;
        for (t = strtok_r(copy, ",", &save); t; t = strtok_r(NULL, ",", &save))
            vec_push(&s->paths, xstrdup(t));
        free(copy);
    } else if ((v = o ? json_get(o, "paths") : NULL) != NULL && v->kind == JV_ARR) {
        size_t i;
        for (i = 0; i < v->len; i++)
            if (v->items[i]->kind == JV_STR)
                vec_push(&s->paths, xstrdup(v->items[i]->str));
    }
}

static void settings_free(Settings *s)
{
    size_t i;
    for (i = 0; i < s->paths.len; i++)
        free(s->paths.data[i]);
    vec_free(&s->paths);
    free(s->cache_dir);
}

/* ---- state ---------------------------------------------------------------------- */

typedef struct IJob {
    LspConfig *cfg;
    long entry;
    uint64_t tukey;
    char *path;
    bool sub;                       /* runs in a child */
    size_t max_bytes;
    uint32_t cancel;                /* atomic: raised by didChange, shutdown, the budget */
    JobGroup g;
    IdxOut out;
} IJob;

enum { EV_SAVE, EV_OPEN, EV_CLOSE, EV_SWEEP, EV_CALL };
typedef struct Event {
    int kind;
    char *path;
    unsigned seq;
    void (*fn)(void *arg, unsigned seq);   /* EV_CALL: run on the scheduler thread */
    void *arg;
} Event;

typedef VEC(Event) EventVec;
typedef VEC(IJob *) IJobVec;

/* What cereal/indexStats reports (a copy, under the lock). */
typedef struct Stats {
    int role;                       /* 0 off, 1 writer, 2 reader */
    bool memory_only;
    size_t tus, fresh, new_, file, miss, lost, backoff;
    size_t running, running_sub;
    uint64_t jobs_inproc, jobs_sub, cancelled, rerouted, fatal, failed, published,
        gen_reloads, man_reloads, sweeps, config_reloads, gc_files, evicted, creates, progress_msgs;
    double rss_anon_mb;
    size_t pi_bytes, pi_names, pi_recs;
} Stats;

static struct Project {
    /* shared, under m */
    Mutex m;
    Cond wake;                      /* the scheduler */
    Cond park;                      /* jobs held (project_hold), until released or cancelled */
    Cond idle_cv;                   /* project_wait */
    bool started, go, stop, ready, hold, idle, pool_ready;
    unsigned paused;                /* project_pause: no job is submitted */
    unsigned sync_req, sync_done, sync_seen;   /* sync_seen: the scheduler's own */
    uint64_t last_edit_ns;
    EventVec events;
    IJobVec done, running;
    Stats st;
    /* the scheduler thread's */
    pthread_t thr;
    Settings set;
    bool progress_cap, create_sent, create_ok, begun, burst, reader_noted;
    LspConfig *cfg;
    Store store;
    PState ps;
    ProjIndex *pi;                  /* the name list (projidx.h); readers: any thread */
    uint64_t last_hint_ns;
    ThreadPool pool;
    bool have_store, writer;
    VEC(char *) open;
    size_t running_n;
    bool sub_busy;
    bool quiescent;                 /* nothing pending or running after the last pass (7.3) */
    uint64_t db_size, db_mtime;
    uint64_t gen_seen;
    uint64_t next_sweep, next_gen, next_lock, next_report, retry_at;
    uint64_t next_evict;
    unsigned writes;                /* manifests written since the last GC */
    size_t burst_total, burst_done, burst_failed;
    Stats cnt;                      /* counters; copied to st each pass */
} P;

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* The server's anonymous resident memory (8.5): not the mapped record files. */
static double rss_anon_mb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    double kb = 0;
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "RssAnon:", 8)) {
            kb = strtod(line + 8, NULL);
            break;
        }
    if (f)
        fclose(f);
    return kb / 1024.0;
}

/* ---- messages to the client ------------------------------------------------------- */

static void send_create(void)
{
    StrBuf sb = {0};
    JsonWriter w;
    rpc_begin(&w, &sb, "window/workDoneProgress/create", CREATE_ID);
    json_key(&w, "token");
    json_str(&w, TOKEN);
    rpc_end(&w, &sb);
    P.create_sent = true;
    P.cnt.creates++;
}

/* kind "begin" (title), "report" or "end"; pct < 0: none. */
static void send_progress(const char *kind, const char *title, const char *message, int pct)
{
    rpc_progress("\"" TOKEN "\"", sizeof TOKEN + 1, kind, title, message, pct);
    P.cnt.progress_msgs++;
}

/* ---- jobs -------------------------------------------------------------------------- */

/* The last line of a child's output: `ok <tukey> <STATUS>` or `error <msg>`. */
static void parse_child(IJob *j, const char *buf, int status, bool timed_out)
{
    const char *end = buf + strlen(buf), *line;
    IdxOut *o = &j->out;
    unsigned long long key;
    char st[32];
    while (end > buf && (end[-1] == '\n' || end[-1] == '\r'))
        end--;
    for (line = end; line > buf && line[-1] != '\n'; line--)
        ;
    if (timed_out) {
        o->result = IDX_ERROR;
        snprintf(o->msg, sizeof o->msg, "timed out after %us", P.set.sub_s);
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
               sscanf(line, "ok %llx %31s", &key, st) == 2 && idx_status_parse(st)) {
        o->result = IDX_DONE;
        o->tukey = key;
        o->tu_status = idx_status_parse(st);
    } else {
        o->result = IDX_ERROR;
        if (WIFSIGNALED(status))
            snprintf(o->msg, sizeof o->msg, "child killed by signal %d", WTERMSIG(status));
        else if (!strncmp(line, "error ", 6))
            snprintf(o->msg, sizeof o->msg, "%.*s", (int)(end - line - 6), line + 6);
        else
            snprintf(o->msg, sizeof o->msg, "child exited %d: %.*s",
                     WIFEXITED(status) ? WEXITSTATUS(status) : -1, (int)(end - line), line);
    }
}

/* CEREAL_FAULT is for this process; the child gets CEREAL_FAULT_CHILD as its own. */
static char **child_env(void)
{
    size_t n = 0, k = 0, i;
    char **env;
    const char *fc = getenv("CEREAL_FAULT_CHILD");
    while (environ[n])
        n++;
    env = xcalloc(n + 2, sizeof *env);
    for (i = 0; i < n; i++)
        if (strncmp(environ[i], "CEREAL_FAULT=", 13) && strncmp(environ[i], "CEREAL_FAULT_CHILD=", 19))
            env[k++] = environ[i];
    if (fc && *fc) {
        char *kv = xmalloc(strlen(fc) + 16);
        sprintf(kv, "CEREAL_FAULT=%s", fc);
        env[k++] = kv;              /* freed by the caller: the only malloc'd one is last */
    }
    env[k] = NULL;
    return env;
}

static void run_child(IJob *j)
{
    char nbuf[24], mbuf[24], parbuf[24], tbuf[24], buf[4096];
    char *argv[24], **env = child_env();
    size_t k = 0, have = 0;
    int pfd[2], status = 0;
    bool timed_out = false, stopping, owned_fault = getenv("CEREAL_FAULT_CHILD") != NULL &&
                                          *getenv("CEREAL_FAULT_CHILD");
    posix_spawn_file_actions_t fa;
    pid_t pid;
    uint64_t deadline = mono_ns() + (uint64_t)P.set.sub_s * 1000000000u;
    const LspConfig *cfg = j->cfg;
    snprintf(nbuf, sizeof nbuf, "%ld", j->entry);
    snprintf(mbuf, sizeof mbuf, "%u", (unsigned)(j->max_bytes >> 20));
    snprintf(parbuf, sizeof parbuf, "%ld", (long)getpid());
    snprintf(tbuf, sizeof tbuf, "%u", (unsigned)P.set.sub_s);
    argv[k++] = (char *)"cereal";
    argv[k++] = (char *)"index-summary";
    argv[k++] = (char *)"--root";
    argv[k++] = (char *)cfg->root;
    if (cfg->db_path) {
        argv[k++] = (char *)"--compdb";
        argv[k++] = (char *)cfg->db_path;
    }
    argv[k++] = (char *)"--cache";
    argv[k++] = P.store.cache;
    argv[k++] = (char *)"--entry";
    argv[k++] = nbuf;
    argv[k++] = (char *)"--budget-mb";
    argv[k++] = mbuf;
    argv[k++] = (char *)"--parent";        /* the child ends with us or at its deadline */
    argv[k++] = parbuf;
    argv[k++] = (char *)"--timeout-s";
    argv[k++] = tbuf;
    argv[k] = NULL;
    memset(&j->out, 0, sizeof j->out);
    mutex_lock(&P.m);
    stopping = P.stop;                      /* no child once the server is stopping */
    mutex_unlock(&P.m);
    if (stopping) {
        j->out.result = IDX_CANCELLED;
        goto out;
    }
    if (pipe(pfd) != 0) {
        j->out.result = IDX_ERROR;
        snprintf(j->out.msg, sizeof j->out.msg, "pipe: %s", strerror(errno));
        goto out;
    }
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, pfd[1], 1);
    posix_spawn_file_actions_adddup2(&fa, pfd[1], 2);
    posix_spawn_file_actions_addclose(&fa, pfd[0]);
    posix_spawn_file_actions_addclose(&fa, pfd[1]);
    if (posix_spawn(&pid, "/proc/self/exe", &fa, NULL, argv, env) != 0) {
        j->out.result = IDX_ERROR;
        snprintf(j->out.msg, sizeof j->out.msg, "cannot start the index process: %s",
                 strerror(errno));
        posix_spawn_file_actions_destroy(&fa);
        close(pfd[0]);
        close(pfd[1]);
        goto out;
    }
    posix_spawn_file_actions_destroy(&fa);
    close(pfd[1]);
    for (;;) {
        struct pollfd p;
        ssize_t n;
        bool stop;
        mutex_lock(&P.m);
        stop = P.stop;
        mutex_unlock(&P.m);
        if (stop || mono_ns() > deadline) {
            timed_out = !stop;
            kill(pid, SIGKILL);
            break;
        }
        p.fd = pfd[0];
        p.events = POLLIN;
        if (poll(&p, 1, 100) <= 0)
            continue;
        if (have + 1 >= sizeof buf) {            /* keep the tail */
            memmove(buf, buf + sizeof buf / 2, have - sizeof buf / 2);
            have -= sizeof buf / 2;
        }
        n = read(pfd[0], buf + have, sizeof buf - 1 - have);
        if (n <= 0)
            break;
        have += (size_t)n;
    }
    buf[have] = 0;
    close(pfd[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    parse_child(j, buf, status, timed_out);
out:
    if (owned_fault) {
        size_t i;
        for (i = 0; env[i]; i++)
            ;
        if (i)
            free(env[i - 1]);
    }
    free(env);
}

static void job_done(void *ctx)
{
    IJob *j = ctx;
    size_t i;
    mutex_lock(&P.m);
    for (i = 0; i < P.running.len; i++)
        if (P.running.data[i] == j) {
            P.running.data[i] = P.running.data[--P.running.len];
            break;
        }
    vec_push(&P.done, j);
    cond_broadcast(&P.wake);
    mutex_unlock(&P.m);
}

/* An in-process job has a smaller budget than a child (8.5): a unit that overruns
 * it is not written but run again in a child. */
static bool retry_over(const IJob *j)
{
    return !j->sub && (size_t)P.set.max_file_mb * 1048576u > j->max_bytes;
}

/* On a pool thread. */
static void ijob_run(void *arg)
{
    IJob *j = arg;
    mutex_lock(&P.m);
    while (P.hold && !P.stop && !(atomic_load_u32(&j->cancel) && !j->sub))
        cond_wait(&P.park, &P.m);       /* test only: cereal/indexHold */
    mutex_unlock(&P.m);
    memset(&j->out, 0, sizeof j->out);
    if (atomic_load_u32(&j->cancel) && !j->sub) {
        j->out.result = IDX_CANCELLED;
    } else if (j->sub) {
        run_child(j);
    } else {
        IdxJob ij;
        memset(&ij, 0, sizeof ij);
        ij.cfg = j->cfg;
        ij.entry = j->entry;
        ij.dir = P.store.dir;
        ij.build_id = P.ps.build_id;
        ij.max_bytes = j->max_bytes;
        ij.cancel = &j->cancel;
        ij.retry_over = retry_over(j);
        index_unit(&ij, &j->out);
    }
}

/* ---- units: selection and classes (8.2) ------------------------------------------------ */

static const char *rel(const char *p)
{
    const char *root = P.cfg->root;
    size_t n = root ? strlen(root) : 0;
    return n && !strncmp(p, root, n) && p[n] == '/' ? p + n + 1 : p;
}

static bool excluded(const CompileEntry *e, void *ctx)
{
    const Settings *s = ctx;
    const char *path = rel(e->abs_file);
    size_t i;
    bool positive = false, hit = false;
    for (i = 0; i < s->paths.len; i++) {
        const char *g = s->paths.data[i];
        if (g[0] == '!') {
            if (!fnmatch(g + 1, path, 0))
                return false;
        } else {
            positive = true;
            hit = hit || !fnmatch(g, path, 0);
        }
    }
    return !positive || hit;
}

static int classify(const PTu *tu)
{
    size_t i;
    uint32_t k;
    for (i = 0; i < P.open.len; i++) {
        const PPath *p;
        if (!strcmp(P.open.data[i], tu->main))
            return 0;
        p = pstate_path(&P.ps, P.open.data[i]);
        for (k = 0; p && tu->man && k < tu->man->nfils; k++)
            if (tu->man->fils[k].p == p)
                return 0;
    }
    return tu->sched.recent ? 1 : 2;
}

/* Notes which units became stale or fresh since the last look. */
static void after_eval(bool startup)
{
    size_t i;
    for (i = 0; i < P.ps.tus.len; i++) {
        PTu *tu = P.ps.tus.data[i];
        PSched *s = &tu->sched;
        if (tu->stale != PS_FRESH) {
            if (!s->pending) {
                s->pending = true;
                s->recent = !startup;
            }
            s->cls = classify(tu);
        } else {
            s->pending = s->recent = false;
            s->next.fails = 0;
            s->next.at = 0;
        }
    }
}

static void reclassify(void)
{
    size_t i;
    for (i = 0; i < P.ps.tus.len; i++)
        if (P.ps.tus.data[i]->stale != PS_FRESH)
            P.ps.tus.data[i]->sched.cls = classify(P.ps.tus.data[i]);
}

static bool tu_before(const PTu *a, const PTu *b)
{
    if (a->sched.cls != b->sched.cls)
        return a->sched.cls < b->sched.cls;
    if (a->sched.cls == 1 && a->stale_ns != b->stale_ns)
        return a->stale_ns > b->stale_ns;
    return a->entry < b->entry;
}

/* 8.5: does this unit go to a child (a FatalTrap before, or no memory for it)? */
static bool route_sub(const PTu *tu, double rss)
{
    double est;
    if (tu->sched.next.sub)
        return true;
    est = 9.0 * (double)P.running_n + 3.0 * (double)(tu->man ? tu->man->bytes_read : 0) / 1048576.0;
    if (!tu->man)
        est = 9.0 * (double)P.running_n + (double)P.set.inproc_bytes / 1048576.0;
    return rss + est > P.set.memory_mb;
}

static PTu *find_tu(uint64_t tukey)
{
    size_t i;
    for (i = 0; i < P.ps.tus.len; i++)
        if (P.ps.tus.data[i]->tukey == tukey)
            return P.ps.tus.data[i];
    return NULL;
}

static size_t count_pending(uint64_t now)
{
    size_t i, n = 0;
    for (i = 0; i < P.ps.tus.len; i++) {
        const PTu *tu = P.ps.tus.data[i];
        if (tu->stale == PS_FRESH)
            continue;
        if (tu->sched.next.at > now) {
            if (tu->sched.next.at != ~(uint64_t)0 &&
                (!P.retry_at || tu->sched.next.at < P.retry_at))
                P.retry_at = tu->sched.next.at;
            continue;
        }
        n++;
    }
    return n;
}

static void submit(PTu *tu, bool sub)
{
    IJob *j = xcalloc(1, sizeof *j);
    size_t hard = (size_t)P.set.max_file_mb * 1048576u;
    j->cfg = P.cfg;
    lsp_config_ref(j->cfg);             /* the job reads it while a reload may replace it */
    j->entry = (long)tu->entry;
    j->tukey = tu->tukey;
    j->path = xstrdup(tu->main);
    j->sub = sub;
    j->max_bytes = sub ? hard : P.set.inproc_bytes;
    group_init(&j->g);
    j->g.on_done = job_done;
    j->g.on_done_ctx = j;
    tu->sched.job = j;
    P.running_n++;
    if (sub)
        P.sub_busy = true;
    if (sub)
        P.cnt.jobs_sub++;
    else
        P.cnt.jobs_inproc++;
    mutex_lock(&P.m);
    vec_push(&P.running, j);
    mutex_unlock(&P.m);
    pool_submit(&P.pool, &j->g, ijob_run, j);
}

static void fail(PTu *tu, const char *what)
{
    PSched *s = &tu->sched;
    char msg[700];
    uint64_t now = mono_ns();
    s->next.fails++;
    s->next.at = s->next.fails == 1 ? now + 60ull * 1000000000u
               : s->next.fails == 2 ? now + 600ull * 1000000000u : ~(uint64_t)0;
    P.cnt.failed++;
    P.burst_failed++;
    snprintf(msg, sizeof msg, "index: %s: %s", rel(tu->main), what);
    rpc_log(2, msg);
}

/* The manifest of tu (every unit's, for NULL) was (re)loaded: the name list and
 * the scheduler's view follow it (10.2).  Loading the records may find one
 * unreadable and make units PS_LOST (pstate_reject_rec), so the stale pass
 * comes last.  The one path by which a job's result, a reader's GEN change, a
 * takeover and a new compile database reach the rest of the server. */
static void publish(PTu *tu, bool startup)
{
    size_t i;
    if (tu) {
        pstate_reload(&P.ps, tu);
        pi_set_tu(P.pi, &P.ps, tu);
    } else {
        for (i = 0; i < P.ps.tus.len; i++)
            pi_set_tu(P.pi, &P.ps, P.ps.tus.data[i]);
        pi_prune(P.pi, &P.ps);
    }
    after_eval(startup);
}

static void reconfigure(void)
{
    pstate_set_config(&P.ps, P.cfg, excluded, &P.set);
    publish(NULL, false);
}

static void after_write(void);

/* A finished job (8.3, 9): publish it by reloading the manifest it wrote. */
static void finish(IJob *j)
{
    PTu *tu = find_tu(j->tukey);
    const IdxOut *o = &j->out;
    P.running_n--;
    if (j->sub)
        P.sub_busy = false;
    if (tu && tu->sched.job == j)       /* a reconfigure may have made it another unit's */
        tu->sched.job = NULL;
    if (j->g.failed) {                  /* a fatal() in the pool: its partial state leaked (8.5) */
        P.cnt.fatal++;
        if (tu && !j->sub)
            tu->sched.next.sub = true;
        else if (tu)
            fail(tu, j->g.msg);
        else
            P.cnt.failed++;
    } else if (o->result == IDX_CANCELLED) {
        P.cnt.cancelled++;
    } else if (o->result == IDX_ERROR) {
        if (tu)
            fail(tu, o->msg);
    } else if (o->tu_status == CTU_OVER_LIMIT && retry_over(j)) {
        if (tu)
            tu->sched.next.sub = true;
        P.cnt.rerouted++;
    } else if (tu) {
        if (o->tukey != j->tukey) {     /* its flags changed: the unit is another one now */
            reconfigure();
        } else {
            publish(tu, false);
            /* The job recorded what it saw, so an include it missed cannot
             * resolve and a record it wrote cannot be unreadable now: if the
             * unit is stale for that, running it again would only repeat it. */
            if (tu->stale == PS_MISS || tu->stale == PS_LOST)
                fail(tu, tu->stale == PS_MISS ? "an include it recorded as missing resolves"
                                              : "a record it wrote cannot be read");
        }
        P.cnt.published++;
        P.burst_done++;
        store_gen_bump(&P.store);
        after_write();
    }
    group_free(&j->g);
    lsp_config_unref(j->cfg);
    free(j->path);
    free(j);
}

/* ---- the store ----------------------------------------------------------------------- */

static void gc(void)
{
    StoreLive live;
    StoreGc g;
    pstate_live(&P.ps, &live);
    store_gc(&P.store, &live, &g);
    P.cnt.gc_files += g.tus + g.recs + g.chunks;
    free((void *)live.tus);
    free((void *)live.recs);
    free((void *)live.chunks);
}

/* 6.4: GC after the startup scan and every GC_EVERY manifest writes; the cache is
 * trimmed to its cap at startup, on taking over and at most every EVICT_MS after. */
static void evict(void)
{
    P.cnt.evicted += store_evict(&P.store, P.set.cache_max_mb);
    P.next_evict = mono_ns() + (uint64_t)EVICT_MS * 1000000u;
}

static void after_write(void)
{
    if (++P.writes < GC_EVERY)
        return;
    P.writes = 0;
    gc();
    if (mono_ns() >= P.next_evict)
        evict();
}

static void report_rejected(void)
{
    size_t i;
    for (i = 0; i < P.ps.log.len; i++) {
        rpc_log(2, P.ps.log.data[i]);
        free(P.ps.log.data[i]);
    }
    P.ps.log.len = 0;
}

static void db_stat(uint64_t *size, uint64_t *mtime)
{
    struct stat st;
    *size = *mtime = 0;
    if (P.cfg->db_path && stat(P.cfg->db_path, &st) == 0) {
        *size = (uint64_t)st.st_size;
        *mtime = (uint64_t)st.st_mtim.tv_sec * 1000000000u + (uint64_t)st.st_mtim.tv_nsec;
    }
}

/* 7.4: the compile database changed on disk. */
static void check_compdb(void)
{
    uint64_t size, mtime;
    LspConfig *n;
    if (!P.cfg->db_path)
        return;
    db_stat(&size, &mtime);
    if (size == P.db_size && mtime == P.db_mtime)
        return;
    n = xcalloc(1, sizeof *n);
    config_init(n, P.cfg->root, P.cfg->db_path);
    if (!n->cmds.len && P.cfg->cmds.len) {      /* mid-rewrite or gone: keep what we have */
        config_free(n);
        free(n);
        return;
    }
    P.db_size = size;
    P.db_mtime = mtime;
    if (config_same(n, P.cfg)) {                /* touched, not changed */
        config_free(n);
        free(n);
        return;
    }
    lsp_config_swap(n);
    P.cfg = n;
    P.cnt.config_reloads++;
    reconfigure();
}

static void reset_backoff(void)
{
    size_t i;
    for (i = 0; i < P.ps.tus.len; i++) {
        P.ps.tus.data[i]->sched.next.fails = 0;
        P.ps.tus.data[i]->sched.next.at = 0;
    }
}

/* Looks at every file and the compile database again (7.3). */
static void sweep_all(void)
{
    size_t changed = pstate_sweep(&P.ps, NULL);
    P.cnt.sweeps++;
    if (changed)
        reset_backoff();
    check_compdb();
    pstate_eval(&P.ps);
    after_eval(false);
}

/* ---- progress (13.1) ---------------------------------------------------------------------- */

static void progress_tick(uint64_t now, size_t pending)
{
    bool work = pending > 0 || P.running_n > 0;
    char msg[96];
    if (P.writer && work && !P.burst) {
        P.burst = true;
        P.burst_total = pending + P.running_n;
        P.burst_done = P.burst_failed = 0;
        P.next_report = 0;
        if (P.progress_cap) {
            if (!P.create_sent)
                send_create();
        } else {
            snprintf(msg, sizeof msg, "indexing %zu translation units", P.burst_total);
            rpc_log(3, msg);
        }
    }
    if (P.burst && work && pending + P.running_n + P.burst_done > P.burst_total)
        P.burst_total = pending + P.running_n + P.burst_done;
    if (P.burst && P.progress_cap && P.create_ok && !P.begun) {
        send_progress("begin", "Indexing", NULL, 0);
        P.begun = true;
    }
    if (P.burst && P.begun && work && now >= P.next_report) {
        snprintf(msg, sizeof msg, "%zu/%zu TUs", P.burst_done, P.burst_total);
        send_progress("report", NULL, msg,
                      P.burst_total ? (int)(100 * P.burst_done / P.burst_total) : 0);
        P.next_report = now + (uint64_t)PROGRESS_MS * 1000000u;
    }
    if (P.burst && !work) {
        P.burst = false;
        snprintf(msg, sizeof msg, "indexed %zu translation units, %zu failed",
                 P.burst_done, P.burst_failed);
        if (P.begun)
            send_progress("end", NULL, msg, -1);
        else if (!P.progress_cap)
            rpc_log(3, msg);
        P.begun = false;
    }
    if (!P.writer && !P.reader_noted && P.have_store) {        /* 6.5: one begin/end */
        static const char note[] = "index maintained by another cereal";
        if (!P.progress_cap) {
            rpc_log(3, note);
            P.reader_noted = true;
        } else if (!P.create_sent) {
            send_create();
        } else if (P.create_ok) {
            send_progress("begin", "Indexing", note, -1);
            send_progress("end", NULL, note, -1);
            P.reader_noted = true;
        }
    }
}

/* ---- the scheduler thread ------------------------------------------------------------------- */

static void startup(void)
{
    char warn[512];
    Settings *s = &P.set;
    uint64_t bid = crec_build_id();
    bool ok = store_open(&P.store, s->cache_dir, P.cfg->root, P.cfg->db_path, bid, warn,
                         sizeof warn);
    if (warn[0])
        rpc_log(2, warn);
    if (!ok)
        return;
    P.have_store = true;
    P.writer = store_lock(&P.store);
    pstate_init(&P.ps, &P.store, bid);
    pstate_set_config(&P.ps, P.cfg, excluded, s);
    publish(NULL, true);
    report_rejected();
    db_stat(&P.db_size, &P.db_mtime);
    P.gen_seen = store_gen(&P.store);
    pool_init(&P.pool, s->threads + 1, 10);
    P.pool.trap_fatal = true;
    mutex_lock(&P.m);
    P.pool_ready = true;
    mutex_unlock(&P.m);
    if (P.writer) {
        gc();
        evict();
    }
}

struct ScopeReq;
static void scope_run(struct ScopeReq *r);

static void do_event(const Event *e)
{
    size_t i;
    switch (e->kind) {
    case EV_SAVE:
        if (!P.have_store)
            break;
        if (pstate_sweep(&P.ps, e->path))
            reset_backoff();
        if (P.cfg->db_path && !strcmp(P.cfg->db_path, e->path))
            check_compdb();
        pstate_eval(&P.ps);
        after_eval(false);
        break;
    case EV_OPEN:
        for (i = 0; i < P.open.len; i++)
            if (!strcmp(P.open.data[i], e->path))
                return;
        vec_push(&P.open, xstrdup(e->path));
        if (P.have_store)
            reclassify();
        break;
    case EV_CLOSE:
        for (i = 0; i < P.open.len; i++)
            if (!strcmp(P.open.data[i], e->path)) {
                free(P.open.data[i]);
                P.open.data[i] = P.open.data[--P.open.len];
                break;
            }
        if (P.have_store)
            reclassify();
        break;
    case EV_CALL:
        e->fn(e->arg, e->seq);
        break;
    case EV_SWEEP:                      /* a query met a file the index did not expect */
        if (P.have_store && mono_ns() - P.last_hint_ns >= (uint64_t)HINT_SWEEP_MIN_MS * 1000000u) {
            P.last_hint_ns = mono_ns();
            sweep_all();
        }
        break;
    }
}

/* Submits what the pool has room for. */
static void submit_pass(uint64_t now, size_t pending, uint64_t last_edit)
{
    bool quiet;
    double rss;
    if (!P.writer || !pending || P.running_n >= (size_t)P.set.threads)
        return;
    mutex_lock(&P.m);
    quiet = P.paused != 0;
    mutex_unlock(&P.m);
    if (quiet)                          /* a rename checks on the pool */
        return;
    quiet = now - last_edit >= (uint64_t)P.set.quiet_ms * 1000000u;
    if (quiet && !lsp_builder_idle())
        quiet = false;
    if (!quiet) {                       /* edits come first (8.4) */
        uint64_t again = last_edit + (uint64_t)P.set.quiet_ms * 1000000u;
        if (again < now + (uint64_t)BUILDER_POLL_MS * 1000000u)
            again = now + (uint64_t)BUILDER_POLL_MS * 1000000u;
        P.retry_at = P.retry_at && P.retry_at < again ? P.retry_at : again;
        return;
    }
    rss = rss_anon_mb();
    P.cnt.rss_anon_mb = rss;
    while (P.running_n < (size_t)P.set.threads) {
        PTu *best = NULL;
        size_t i;
        for (i = 0; i < P.ps.tus.len; i++) {
            PTu *tu = P.ps.tus.data[i];
            if (tu->stale == PS_FRESH || tu->sched.job || tu->sched.next.at > now)
                continue;
            if (P.sub_busy && route_sub(tu, rss))
                continue;               /* one child at a time */
            if (!best || tu_before(tu, best))
                best = tu;
        }
        if (!best)
            break;
        submit(best, route_sub(best, rss));
    }
}

static void publish_stats(bool idle)
{
    size_t i;
    Stats s = P.cnt;
    s.role = !P.have_store ? 0 : P.writer ? 1 : 2;
    s.memory_only = P.have_store && P.store.private_root;
    s.tus = P.ps.tus.len;
    for (i = 0; i < P.ps.tus.len; i++) {
        const PTu *tu = P.ps.tus.data[i];
        switch (tu->stale) {
        case PS_FRESH: s.fresh++; break;
        case PS_NEW: s.new_++; break;
        case PS_FILE: s.file++; break;
        case PS_MISS: s.miss++; break;
        case PS_LOST: s.lost++; break;
        }
        if (tu->sched.next.fails)
            s.backoff++;
    }
    s.running = P.running_n;
    s.running_sub = P.sub_busy;
    mutex_lock(&P.m);
    P.st = s;
    P.idle = idle;
    if (P.sync_seen > P.sync_done)
        P.sync_done = P.sync_seen;
    P.ready = true;
    cond_broadcast(&P.idle_cv);
    mutex_unlock(&P.m);
}

static void *sched_main(void *arg)
{
    bool first = true, stopped;              /* one pass at once: publish the first stats, find work */
    (void)arg;
    mutex_lock(&P.m);
    while (!P.go && !P.stop)
        cond_wait(&P.wake, &P.m);
    stopped = P.stop;
    mutex_unlock(&P.m);
    if (stopped)
        return NULL;
    startup();
    P.next_sweep = mono_ns() + (uint64_t)(P.set.sweep_s * 1e9);
    P.next_gen = mono_ns() + (uint64_t)GEN_POLL_MS * 1000000u;
    P.next_lock = mono_ns() + (uint64_t)(P.set.lock_retry_s * 1e9);
    for (;;) {
        EventVec evs = {0};
        IJobVec dn = {0};
        uint64_t now, last_edit, wake;
        size_t i, pending;
        bool idle, quiescent;
        mutex_lock(&P.m);
        for (;;) {
            now = mono_ns();
            if (first || P.stop || P.events.len || P.done.len)
                break;
            wake = now + 3600ull * 1000000000u;
            if (P.have_store) {
                if (P.retry_at && P.retry_at < wake)
                    wake = P.retry_at;
                if (!P.writer && P.next_gen < wake)
                    wake = P.next_gen;
                if (!P.writer && P.next_lock < wake)
                    wake = P.next_lock;
                if (P.writer && P.quiescent && P.next_sweep < wake)
                    wake = P.next_sweep;    /* only a quiescent writer sweeps: else it would spin */
            }
            if (wake <= now)
                break;
            {
                struct timespec dl = cond_deadline((double)(wake - now) * 1e-9);
                cond_timedwait(&P.wake, &P.m, &dl);
            }
        }
        first = false;
        if (P.stop) {
            mutex_unlock(&P.m);
            break;
        }
        evs = P.events;
        memset(&P.events, 0, sizeof P.events);
        dn = P.done;
        memset(&P.done, 0, sizeof P.done);
        last_edit = P.last_edit_ns;
        mutex_unlock(&P.m);
        now = mono_ns();
        for (i = 0; i < dn.len; i++)
            finish(dn.data[i]);
        vec_free(&dn);
        for (i = 0; i < evs.len; i++) {
            do_event(&evs.data[i]);
            free(evs.data[i].path);
        }
        vec_free(&evs);
        if (P.have_store) {
            P.retry_at = 0;         /* count_pending and submit_pass lower it */
            pending = count_pending(now);
            quiescent = pending == 0 && P.running_n == 0;
            if (P.writer && quiescent && now >= P.next_sweep) {
                sweep_all();
                P.next_sweep = now + (uint64_t)(P.set.sweep_s * 1e9);
                pending = count_pending(now);
                quiescent = pending == 0 && P.running_n == 0;
            }
            if (!P.writer && now >= P.next_gen) {   /* 6.5: readers poll GEN */
                uint64_t g = store_gen(&P.store);
                P.next_gen = now + (uint64_t)GEN_POLL_MS * 1000000u;
                if (g != P.gen_seen) {
                    P.gen_seen = g;
                    for (i = 0; i < P.ps.tus.len; i++)
                        P.cnt.man_reloads += pstate_refresh(&P.ps, P.ps.tus.data[i]);
                    publish(NULL, false);
                    P.cnt.gen_reloads++;
                }
            }
            if (!P.writer && now >= P.next_lock) {  /* ... and take over from a writer that left */
                P.next_lock = now + (uint64_t)(P.set.lock_retry_s * 1e9);
                if (store_lock(&P.store)) {
                    P.writer = true;
                    for (i = 0; i < P.ps.tus.len; i++)
                        P.cnt.man_reloads += pstate_refresh(&P.ps, P.ps.tus.data[i]);
                    publish(NULL, false);
                    gc();
                    evict();
                }
            }
            pending = count_pending(now);
            submit_pass(now, pending, last_edit);
            pending = count_pending(now);
            report_rejected();      /* what the loads found unreadable since the last pass (6.3) */
            progress_tick(now, pending);
            P.quiescent = P.running_n == 0 && pending == 0;
            idle = !P.writer || P.quiescent;    /* a reader indexes nothing: nothing to wait for */
        } else {
            idle = true;
        }
        publish_stats(idle);
    }
    mutex_lock(&P.m);
    P.pool_ready = false;
    mutex_unlock(&P.m);
    pool_free(&P.pool);                 /* the jobs end at once: cancelled or killed */
    {
        size_t i;
        mutex_lock(&P.m);
        for (i = 0; i < P.done.len; i++)
            P.running_n--;
        mutex_unlock(&P.m);
    }
    return NULL;
}

/* ---- rename support (B5d, 11.5) ---------------------------------------------------------------- */

static void post(int kind, const char *path, unsigned seq);
static void post_call(void (*fn)(void *arg, unsigned seq), void *arg, unsigned seq);
static void on_sync(void *arg, unsigned seq);
static void on_created(void *arg, unsigned seq);
static void on_scope(void *arg, unsigned seq);

typedef struct ScopeReq {
    char **files;                   /* sorted */
    size_t nfiles;
    ProjScope out;
    bool done;
    int refs;                       /* the caller and the scheduler's event; P.m */
} ScopeReq;

static int str_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void scope_unref(ScopeReq *r)    /* P.m held */
{
    size_t i;
    if (--r->refs)
        return;
    for (i = 0; i < r->nfiles; i++)
        free(r->files[i]);
    free(r->files);
    project_scope_free(&r->out);
    free(r);
}

static bool lists_file(const PMan *m, char *const *files, size_t n)
{
    uint32_t i;
    for (i = 0; i < m->nfils; i++) {
        const char *p = m->fils[i].p->path;
        if (bsearch(&p, files, n, sizeof *files, str_cmp))
            return true;
    }
    return false;
}

/* The units a rename of the files' names must check, and whether it may go
 * ahead: R2 (the index is complete: a unit that is not indexed could use any
 * external name) and R5.  Scheduler thread. */
static void do_scope(ScopeReq *r)
{
    ProjScope *o = &r->out;
    StrBuf why = {0}, m = {0};
    size_t i, bad = 0;
    if (!P.have_store) {
        o->refuse = xstrdup("the project index has no store (indexing is not running)");
        return;
    }
    for (i = 0; i < P.ps.tus.len; i++) {
        const PTu *tu = P.ps.tus.data[i];
        const char *state = tu->stale != PS_FRESH ? pstate_why(tu->stale)
                          : !tu->man ? "not indexed"
                          : tu->sched.job ? "being indexed"
                          : tu->man->status == CTU_FAILED ? "its index failed"
                          : tu->man->status == CTU_OVER_LIMIT ? "over the size limit"
                          : NULL;
        bool listed = !state && lists_file(tu->man, r->files, r->nfiles);
        char nerr[64];
        if (listed && tu->man->status == CTU_PARTIAL) {
            snprintf(nerr, sizeof nerr, "only partly indexed: %u errors in the check",
                     (unsigned)tu->man->errors);
            state = nerr;
        }
        if (state) {
            if (bad++ < 3)
                sb_printf(&why, "%s%s (%s)", bad > 1 ? ", " : "", rel(tu->main), state);
        } else if (listed) {
            o->mains = xrealloc(o->mains, (o->nmains + 1) * sizeof *o->mains);
            o->entries = xrealloc(o->entries, (o->nmains + 1) * sizeof *o->entries);
            o->entries[o->nmains] = tu->entry;
            o->mains[o->nmains++] = xstrdup(tu->main);
        }
    }
    if (bad)
        sb_printf(&m, "the project index is incomplete: %zu of %zu translation units "
                  "are not fully indexed (%s%s); renaming needs all of them. "
                  "Fix the units, or exclude those that never index with "
                  "cereal.index.paths",
                  bad, P.ps.tus.len, sb_cstr(&why), bad > 3 ? ", ..." : "");
    else if (o->nmains > P.set.rename_max)
        sb_printf(&m, "%zu translation units are affected, more than "
                  "cereal.index.renameMaxTUs (%zu)", o->nmains, P.set.rename_max);
    if (m.len)
        o->refuse = (char *)sb_cstr(&m);
    sb_free(&why);
}

static void scope_run(ScopeReq *r)
{
    do_scope(r);
    mutex_lock(&P.m);
    r->done = true;
    cond_broadcast(&P.idle_cv);
    scope_unref(r);
    mutex_unlock(&P.m);
}

void project_scope_free(ProjScope *s)
{
    size_t i;
    for (i = 0; i < s->nmains; i++)
        free(s->mains[i]);
    free(s->mains);
    free(s->entries);
    free(s->refuse);
    memset(s, 0, sizeof *s);
}

bool project_scope(const char *const *files, size_t nfiles, unsigned timeout_ms,
                   ProjScope *out)
{
    ScopeReq *r;
    Event e;
    size_t i;
    bool ok = false;
    memset(out, 0, sizeof *out);
    if (!P.started)
        return false;
    r = xcalloc(1, sizeof *r);
    r->files = xmalloc((nfiles + 1) * sizeof *r->files);
    for (i = 0; i < nfiles; i++)
        r->files[i] = xstrdup(files[i]);
    r->nfiles = nfiles;
    qsort(r->files, nfiles, sizeof *r->files, str_cmp);
    r->refs = 2;
    e.kind = EV_CALL;
    e.path = NULL;
    e.seq = 0;
    e.fn = on_scope;
    e.arg = r;
    mutex_lock(&P.m);
    P.go = true;
    vec_push(&P.events, e);
    cond_broadcast(&P.wake);
    {
        struct timespec dl = cond_deadline(timeout_ms / 1000.0);
        while (!r->done && !P.stop) {
            if (!timeout_ms)
                cond_wait(&P.idle_cv, &P.m);
            else if (!cond_timedwait(&P.idle_cv, &P.m, &dl))
                break;
        }
    }
    if (r->done) {
        *out = r->out;                  /* the caller owns what it holds now */
        memset(&r->out, 0, sizeof r->out);
        ok = true;
    }
    scope_unref(r);
    mutex_unlock(&P.m);
    return ok;
}

void project_pause(bool on)
{
    if (!P.started)
        return;
    mutex_lock(&P.m);
    P.paused += on ? 1 : -1;
    mutex_unlock(&P.m);
    if (!on)
        post_call(on_sync, NULL, 0);    /* wakes the scheduler to submit again */
}

typedef struct GateTask {
    void (*fn)(void *arg, size_t i);
    void *arg;
    size_t i;
} GateTask;

static void gate_task(void *p)
{
    GateTask *t = p;
    t->fn(t->arg, t->i);
}

void project_gate(void (*fn)(void *arg, size_t i), void *arg, size_t n)
{
    GateTask *t = xmalloc((n + 1) * sizeof *t);
    JobGroup g;
    size_t i;
    bool pool;
    mutex_lock(&P.m);
    pool = P.pool_ready && !P.stop;
    mutex_unlock(&P.m);
    for (i = 0; i < n; i++) {
        t[i].fn = fn;
        t[i].arg = arg;
        t[i].i = i;
    }
    if (!pool) {                        /* no pool (indexing off): in turn, here */
        for (i = 0; i < n; i++)
            fn(arg, i);
        free(t);
        return;
    }
    group_init(&g);
    for (i = 0; i < n; i++)
        pool_submit(&P.pool, &g, gate_task, &t[i]);
    group_wait(&P.pool, &g);
    group_free(&g);
    free(t);
}

/* ---- the API --------------------------------------------------------------------------------- */

bool project_start(const JsonValue *params, LspConfig *cfg)
{
    Settings s;
    memset(&P, 0, sizeof P);
    load_settings(&s, params, cfg);
    if (!s.enable || !cfg->root) {
        settings_free(&s);
        return false;
    }
    P.set = s;
    P.pi = pi_new();
    P.cfg = cfg;
    P.progress_cap = json_bool_of(json_path(params, "capabilities.window.workDoneProgress"),
                                  false);
    mutex_init(&P.m);
    cond_init(&P.wake);
    cond_init(&P.park);
    cond_init(&P.idle_cv);
    if (pthread_create(&P.thr, NULL, sched_main, NULL) != 0)
        fatal("cannot start the index scheduler thread");
    P.started = true;
    return true;
}

static void post(int kind, const char *path, unsigned seq)
{
    Event e;
    e.kind = kind;
    e.path = path ? xstrdup(path) : NULL;
    e.seq = seq;
    e.fn = NULL;
    e.arg = NULL;
    mutex_lock(&P.m);
    vec_push(&P.events, e);
    cond_broadcast(&P.wake);
    mutex_unlock(&P.m);
}

/* Run fn on the scheduler thread, in order with the other events. */
static void post_call(void (*fn)(void *arg, unsigned seq), void *arg, unsigned seq)
{
    Event e;
    e.kind = EV_CALL;
    e.path = NULL;
    e.seq = seq;
    e.fn = fn;
    e.arg = arg;
    mutex_lock(&P.m);
    vec_push(&P.events, e);
    cond_broadcast(&P.wake);
    mutex_unlock(&P.m);
}

static void on_sync(void *arg, unsigned seq)       /* a barrier: seen once the pass publishes */
{
    (void)arg;
    if (seq > P.sync_seen)
        P.sync_seen = seq;
}

static void on_created(void *arg, unsigned seq)    /* the client took the progress token */
{
    (void)arg;
    (void)seq;
    P.create_ok = true;
}

static void on_scope(void *arg, unsigned seq)
{
    (void)seq;
    scope_run(arg);
}

void project_go(void)
{
    if (!P.started)
        return;
    mutex_lock(&P.m);
    P.go = true;
    cond_broadcast(&P.wake);
    mutex_unlock(&P.m);
}

void project_doc(const char *path, bool open)
{
    if (P.started)
        post(open ? EV_OPEN : EV_CLOSE, path, 0);
}

void project_save(const char *path)
{
    if (P.started)
        post(EV_SAVE, path, 0);
}

void project_edit(void)
{
    size_t i;
    if (!P.started)
        return;
    mutex_lock(&P.m);
    P.last_edit_ns = mono_ns();
    for (i = 0; i < P.running.len; i++)
        if (!P.running.data[i]->sub)
            atomic_store_u32(&P.running.data[i]->cancel, 1);
    cond_broadcast(&P.park);
    mutex_unlock(&P.m);
}

bool project_response(const JsonValue *id)
{
    if (!P.started || id->kind != JV_STR || strcmp(id->str, CREATE_ID))
        return false;
    post_call(on_created, NULL, 0);
    return true;
}

void project_hold(bool hold)
{
    if (!P.started)
        return;
    mutex_lock(&P.m);
    P.hold = hold;
    cond_broadcast(&P.park);
    mutex_unlock(&P.m);
}

void project_wait(void)
{
    unsigned n;
    Event e;
    if (!P.started)
        return;
    mutex_lock(&P.m);
    P.go = true;
    n = ++P.sync_req;
    e.kind = EV_CALL;
    e.path = NULL;
    e.seq = n;
    e.fn = on_sync;
    e.arg = NULL;
    vec_push(&P.events, e);
    cond_broadcast(&P.wake);
    while (!P.stop && !(P.ready && P.sync_done >= n && P.idle))
        cond_wait(&P.idle_cv, &P.m);
    mutex_unlock(&P.m);
}

void project_stats(JsonWriter *w)
{
    Stats s;
    if (P.started) {
        mutex_lock(&P.m);
        s = P.st;
        mutex_unlock(&P.m);
        PiStats ps = pi_stats(P.pi);
        s.pi_bytes = ps.bytes;
        s.pi_names = ps.names;
        s.pi_recs = ps.recs;
    } else {
        memset(&s, 0, sizeof s);
    }
#define N(K, V) json_key(w, K), json_int(w, (long long)(V))
    json_begin_object(w);
    json_key(w, "enabled");
    json_bool(w, P.started);
    json_key(w, "role");
    json_str(w, s.role == 1 ? "writer" : s.role == 2 ? "reader" : "off");
    json_key(w, "memoryOnly");
    json_bool(w, s.memory_only);
    N("tus", s.tus);
    N("fresh", s.fresh);
    N("new", s.new_);
    N("file", s.file);
    N("miss", s.miss);
    N("lost", s.lost);
    N("backoff", s.backoff);
    N("running", s.running);
    N("runningSub", s.running_sub);
    N("jobsInProcess", s.jobs_inproc);
    N("jobsSubprocess", s.jobs_sub);
    N("cancelled", s.cancelled);
    N("rerouted", s.rerouted);
    N("fatal", s.fatal);
    N("failed", s.failed);
    N("published", s.published);
    N("genReloads", s.gen_reloads);
    N("sweeps", s.sweeps);
    N("configReloads", s.config_reloads);
    N("manReloads", s.man_reloads);
    N("gcFiles", s.gc_files);
    N("evicted", s.evicted);
    N("progressCreates", s.creates);
    N("progressMessages", s.progress_msgs);
    N("rssAnonMB", s.rss_anon_mb);
    N("nameListBytes", s.pi_bytes);
    N("names", s.pi_names);
    N("records", s.pi_recs);
    json_end_object(w);
#undef N
}

struct ProjIndex *project_index(void)
{
    return P.started ? P.pi : NULL;
}

void project_hint_sweep(void)
{
    if (P.started)
        post(EV_SWEEP, NULL, 0);
}

void project_stop(void)
{
    size_t i;
    if (!P.started)
        return;
    mutex_lock(&P.m);
    P.stop = true;
    for (i = 0; i < P.running.len; i++)
        atomic_store_u32(&P.running.data[i]->cancel, 1);
    cond_broadcast(&P.wake);
    cond_broadcast(&P.park);
    cond_broadcast(&P.idle_cv);
    mutex_unlock(&P.m);
    pthread_join(P.thr, NULL);
    for (i = 0; i < P.done.len; i++) {
        IJob *j = P.done.data[i];
        group_free(&j->g);
        lsp_config_unref(j->cfg);
        free(j->path);
        free(j);
    }
    vec_free(&P.done);
    vec_free(&P.running);
    for (i = 0; i < P.events.len; i++)
        free(P.events.data[i].path);
    vec_free(&P.events);
    for (i = 0; i < P.open.len; i++)
        free(P.open.data[i]);
    vec_free(&P.open);
    if (P.have_store) {
        pstate_free(&P.ps);
        store_close(&P.store);
    }
    pi_free(P.pi);
    P.pi = NULL;
    settings_free(&P.set);
    P.started = false;
}
