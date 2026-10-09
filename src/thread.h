/* thread.h - threads, locks, atomics and a job pool.
 *
 * C99 has no threads; this wraps POSIX threads.  Atomics use the GCC/Clang
 * __atomic builtins when available and fall back to one global mutex
 * otherwise (correct, slow), so the code stays compilable by compilers that
 * only know C99 - including cereal itself. */
#ifndef CEREAL_THREAD_H
#define CEREAL_THREAD_H

#include "common.h"

#include <pthread.h>
#include <time.h>

typedef pthread_mutex_t Mutex;
typedef pthread_cond_t Cond;

void mutex_init(Mutex *m);
void mutex_destroy(Mutex *m);
void mutex_lock(Mutex *m);
void mutex_unlock(Mutex *m);
void cond_init(Cond *c);
void cond_destroy(Cond *c);
void cond_wait(Cond *c, Mutex *m);
void cond_broadcast(Cond *c);
/* The time `seconds` from now, for cond_timedwait. */
struct timespec cond_deadline(double seconds);
/* cond_wait, but at most until deadline; false once it has passed. */
bool cond_timedwait(Cond *c, Mutex *m, const struct timespec *deadline);

int cpu_count(void);

/* ---- atomics (typed; acquire loads, release stores) ------------------ */

#if defined(__GNUC__) && !defined(CEREAL_NO_ATOMIC_BUILTINS)
#define CEREAL_ATOMIC_BUILTINS 1
static inline uint32_t atomic_load_u32(const uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline void atomic_store_u32(uint32_t *p, uint32_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
static inline uint32_t atomic_add_u32(uint32_t *p, uint32_t v)
{
    return __atomic_fetch_add(p, v, __ATOMIC_ACQ_REL);
}
static inline size_t atomic_load_size(const size_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline void atomic_store_size(size_t *p, size_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
static inline void *atomic_load_ptr(void *const *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline void atomic_store_ptr(void **p, void *v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
/* Returns true if *p was `expected` and is now `desired`. */
static inline bool atomic_cas_ptr(void **p, void *expected, void *desired)
{
    return __atomic_compare_exchange_n(p, &expected, desired, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
#else
uint32_t atomic_load_u32(const uint32_t *p);
void atomic_store_u32(uint32_t *p, uint32_t v);
uint32_t atomic_add_u32(uint32_t *p, uint32_t v);
size_t atomic_load_size(const size_t *p);
void atomic_store_size(size_t *p, size_t v);
void *atomic_load_ptr(void *const *p);
void atomic_store_ptr(void **p, void *v);
bool atomic_cas_ptr(void **p, void *expected, void *desired);
#endif

/* ---- threads and the job pool --------------------------------------- */

typedef void (*JobFn)(void *arg);

typedef struct Job {
    JobFn fn;
    void *arg;
    struct JobGroup *group;
} Job;

typedef struct JobGroup {
    Mutex m;
    Cond done;
    size_t pending;
} JobGroup;

typedef struct ThreadPool {
    Mutex m;
    Cond work;
    VEC(Job) queue;          /* FIFO: head index into queue */
    size_t head;
    pthread_t *threads;
    int nthreads;
    bool stop;
} ThreadPool;

/* nthreads <= 0 means "cpu_count()". */
void pool_init(ThreadPool *p, int nthreads);
void pool_free(ThreadPool *p);

void group_init(JobGroup *g);
void group_free(JobGroup *g);
void pool_submit(ThreadPool *p, JobGroup *g, JobFn fn, void *arg);
/* Wait for the group; the calling thread helps run queued jobs. */
void group_wait(ThreadPool *p, JobGroup *g);
/* Run one queued job on the calling thread; false if none was queued. */
bool pool_run_one(ThreadPool *p);

#endif
