/* thread.c - threads, locks and a job pool. */
#include "thread.h"

#include <string.h>

#include <unistd.h>

void mutex_init(Mutex *m)
{
    if (pthread_mutex_init(m, NULL) != 0)
        fatal("pthread_mutex_init failed");
}

void mutex_destroy(Mutex *m) { pthread_mutex_destroy(m); }
void mutex_lock(Mutex *m)
{
    pthread_mutex_lock(m);
    fatal_locks_held++;
}

void mutex_unlock(Mutex *m)
{
    fatal_locks_held--;
    pthread_mutex_unlock(m);
}

void cond_init(Cond *c)
{
    if (pthread_cond_init(c, NULL) != 0)
        fatal("pthread_cond_init failed");
}

void cond_destroy(Cond *c) { pthread_cond_destroy(c); }
void cond_wait(Cond *c, Mutex *m) { pthread_cond_wait(c, m); }
void cond_broadcast(Cond *c) { pthread_cond_broadcast(c); }

struct timespec cond_deadline(double seconds)
{
    struct timespec ts;     /* pthread_cond_timedwait's default clock */
    long ns;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)seconds;
    ns = ts.tv_nsec + (long)((seconds - (double)(time_t)seconds) * 1e9);
    ts.tv_sec += ns / 1000000000L;
    ts.tv_nsec = ns % 1000000000L;
    return ts;
}

bool cond_timedwait(Cond *c, Mutex *m, const struct timespec *deadline)
{
    return pthread_cond_timedwait(c, m, deadline) == 0;
}

#ifndef CEREAL_ATOMIC_BUILTINS
/* Correct but slow: every atomic operation serializes on one lock. */
static pthread_mutex_t atomic_lock = PTHREAD_MUTEX_INITIALIZER;
#define FB(stmt)                                                           \
    do {                                                                   \
        pthread_mutex_lock(&atomic_lock);                                  \
        stmt;                                                              \
        pthread_mutex_unlock(&atomic_lock);                                \
    } while (0)
uint32_t atomic_load_u32(const uint32_t *p) { uint32_t v; FB(v = *p); return v; }
void atomic_store_u32(uint32_t *p, uint32_t v) { FB(*p = v); }
uint32_t atomic_add_u32(uint32_t *p, uint32_t v) { uint32_t o; FB((o = *p, *p = o + v)); return o; }
size_t atomic_load_size(const size_t *p) { size_t v; FB(v = *p); return v; }
void atomic_store_size(size_t *p, size_t v) { FB(*p = v); }
void *atomic_load_ptr(void *const *p) { void *v; FB(v = *p); return v; }
void atomic_store_ptr(void **p, void *v) { FB(*p = v); }
bool atomic_cas_ptr(void **p, void *e, void *d)
{
    bool ok;
    FB((ok = *p == e) ? (void)(*p = d) : (void)0);
    return ok;
}
#endif

int cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

/* ---- pool ----------------------------------------------------------- */

static bool take_job(ThreadPool *p, Job *out)
{
    if (p->head < p->queue.len) {
        *out = p->queue.data[p->head++];
        if (p->head == p->queue.len)
            p->head = p->queue.len = 0;
        return true;
    }
    return false;
}

/* Runs a job and marks it done; see ThreadPool.trap_fatal. */
static void run_job(ThreadPool *p, Job *j)
{
    JobGroup *g = j->group;
    FatalTrap tr;
    bool failed = false;
    if (!p->trap_fatal) {
        j->fn(j->arg);
    } else {
        fatal_trap_push(&tr);
        if (setjmp(tr.jb) == 0) {
            j->fn(j->arg);
            fatal_trap_pop(&tr);
        } else {
            failed = true;
        }
    }
    mutex_lock(&g->m);
    if (failed && !g->failed) {
        g->failed = true;
        memcpy(g->msg, tr.msg, sizeof g->msg);
    }
    if (--g->pending == 0)
        cond_broadcast(&g->done);
    mutex_unlock(&g->m);
}

static void *worker_main(void *arg)
{
    ThreadPool *p = arg;
    for (;;) {
        Job j;
        mutex_lock(&p->m);
        while (!p->stop && !take_job(p, &j))
            cond_wait(&p->work, &p->m);
        if (p->stop) {
            mutex_unlock(&p->m);
            return NULL;
        }
        mutex_unlock(&p->m);
        run_job(p, &j);
    }
}

void pool_init(ThreadPool *p, int nthreads)
{
    int i;
    memset(p, 0, sizeof *p);
    if (nthreads <= 0)
        nthreads = cpu_count();
    mutex_init(&p->m);
    cond_init(&p->work);
    /* the submitting thread also runs jobs (group_wait), so spawn n-1 */
    p->nthreads = nthreads - 1;
    p->threads = xcalloc((size_t)(p->nthreads > 0 ? p->nthreads : 1),
                         sizeof(pthread_t));
    for (i = 0; i < p->nthreads; i++)
        if (pthread_create(&p->threads[i], NULL, worker_main, p) != 0)
            fatal("pthread_create failed");
}

void pool_free(ThreadPool *p)
{
    int i;
    mutex_lock(&p->m);
    p->stop = true;
    cond_broadcast(&p->work);
    mutex_unlock(&p->m);
    for (i = 0; i < p->nthreads; i++)
        pthread_join(p->threads[i], NULL);
    free(p->threads);
    vec_free(&p->queue);
    cond_destroy(&p->work);
    mutex_destroy(&p->m);
}

void group_init(JobGroup *g)
{
    mutex_init(&g->m);
    cond_init(&g->done);
    g->pending = 0;
    g->failed = false;
    g->msg[0] = 0;
}

void group_free(JobGroup *g)
{
    cond_destroy(&g->done);
    mutex_destroy(&g->m);
}

void pool_submit(ThreadPool *p, JobGroup *g, JobFn fn, void *arg)
{
    Job j;
    j.fn = fn;
    j.arg = arg;
    j.group = g;
    mutex_lock(&g->m);
    g->pending++;
    mutex_unlock(&g->m);
    mutex_lock(&p->m);
    vec_push(&p->queue, j);
    cond_broadcast(&p->work);
    mutex_unlock(&p->m);
}

bool pool_run_one(ThreadPool *p)
{
    Job j;
    bool got;
    mutex_lock(&p->m);
    got = take_job(p, &j);
    mutex_unlock(&p->m);
    if (got)
        run_job(p, &j);
    return got;
}

void group_wait(ThreadPool *p, JobGroup *g)
{
    for (;;) {
        Job j;
        bool got;
        mutex_lock(&g->m);
        if (g->pending == 0) {
            mutex_unlock(&g->m);
            break;
        }
        mutex_unlock(&g->m);
        mutex_lock(&p->m);
        got = take_job(p, &j);
        mutex_unlock(&p->m);
        if (got) {
            run_job(p, &j);
            continue;
        }
        mutex_lock(&g->m);
        while (g->pending != 0)
            cond_wait(&g->done, &g->m);
        mutex_unlock(&g->m);
        break;
    }
    if (g->failed) /* every job has ended: no one else writes it */
        fatal("%s", g->msg);
}

/* ---- fault injection --------------------------------------------------- */

static pthread_once_t fault_once = PTHREAD_ONCE_INIT;
static char fault_site[64];
static uint32_t fault_at, fault_count;

static void fault_init(void)
{
    const char *e = getenv("CEREAL_FAULT"), *colon;
    size_t n;
    if (!e || !*e)
        return;
    colon = strchr(e, ':');
    n = colon ? (size_t)(colon - e) : strlen(e);
    if (n >= sizeof fault_site)
        return;
    memcpy(fault_site, e, n);
    fault_at = colon ? (uint32_t)strtoul(colon + 1, NULL, 10) : 1;
}

bool fault_hit(const char *site)
{
    pthread_once(&fault_once, fault_init);
    return fault_site[0] && !strcmp(fault_site, site) &&
           atomic_add_u32(&fault_count, 1) + 1 == fault_at;
}
