/* sweep_rt.h - the gcc side of `cereal check --eval-sweep` (docs/EVAL_DESIGN.md).
 *
 * The sweep prints, per function, a thunk `unsigned t_NAME(in, out)`: it rebuilds the graph of objects
 * the call made (sw_load), calls NAME, and packs the result bytes (the return value, a returned
 * pointer as a tag, then the final contents of every object).  sw_main runs the
 * calls of a file of `NAME INHEX` lines through the table of thunks and prints each result as
 * hex, one line per call ("-" is empty). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef SW_FORK
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* The call line carries the graph of objects the call made (docs/LAZY_BLOBS_DESIGN.md):
 *   u32 n; u32 size[n]; the arguments; u32 nlinks, {blob, off, target + 1, target off}[];
 *   u32 nzero, {blob, off}[]; the initial bytes of each blob.
 * sw_load rebuilds it (one arena, each blob SW_SLACK + 16 bytes longer, so a one-past-the-end pointer is not the next
 * blob's start) and returns the arguments; sw_out appends the final bytes of every blob, with the
 * link and zero cells zeroed (pointer values differ between the two sides), and frees the graph. */
/* a returned pointer may point this far past a blob (64 + LHEAD of src/c/eval_run.c) and still be tagged */
#define SW_SLACK 192

/* a link whose target has the top bit set names a function: the index in the evaluator's function table, which the
 * driver fills (sw_fp_N, from the "fp" lines of the sweep); weak, so the drivers of all files share one table */
__attribute__((weak)) void *sw_fn[1 << 16];

/* the live call's arena: all its blobs are in it, so the driver's `-Wl,--wrap=free` and `--wrap=realloc` (__wrap_free, __wrap_realloc in drvmain.c) can ignore a free of a pointer into one
 * and return a realloc of one unchanged (a lazy blob is never freed and stays where it is, on either side: docs/LAZY_HEAP_DESIGN.md,
 * docs/LAZY_REALLOC_DESIGN.md) and pass the function's own frees and reallocs on */
__attribute__((weak)) unsigned char *sw_alo, *sw_ahi;

typedef struct SwG {
    unsigned n, nl, nz;
    unsigned char *arena, **b;
    unsigned *size;
    const unsigned char *lk, *zc;
} SwG;

static unsigned sw_u32(const unsigned char *p)
{
    unsigned x;
    memcpy(&x, p, 4);
    return x;
}

static const unsigned char *sw_load(SwG *g, const unsigned char *in, unsigned argbytes)
{
    unsigned i;
    size_t total = 0;
    const unsigned char *p = in + 4, *args, *ini;
    g->n = sw_u32(in);
    g->b = malloc((g->n + 1) * sizeof *g->b);
    g->size = malloc((g->n + 1) * sizeof *g->size);
    for (i = 0; i < g->n; i++)
        g->size[i] = sw_u32(p + 4 * i);
    p += 4 * g->n;
    args = p;
    p += argbytes;
    g->nl = sw_u32(p);
    g->lk = p + 4;
    p += 4 + 16 * g->nl;
    g->nz = sw_u32(p);
    g->zc = p + 4;
    ini = p + 4 + 8 * g->nz;
    for (i = 0; i < g->n; i++)
        total += (g->size[i] + SW_SLACK + 16 + 15) & ~15u;
    g->arena = calloc(total + 16, 1);
    sw_alo = g->arena;
    sw_ahi = g->arena + total;
    total = 0;
    for (i = 0; i < g->n; i++) {
        g->b[i] = g->arena + total;
        total += (g->size[i] + SW_SLACK + 16 + 15) & ~15u;
        memcpy(g->b[i], ini, g->size[i]);
        ini += g->size[i];
    }
    for (i = 0; i < g->nl; i++) {
        const unsigned char *l = g->lk + 16 * i;
        unsigned tb = sw_u32(l + 8);
        void *t = tb & 0x80000000u ? sw_fn[tb & 0x7fffffffu] : tb ? (void *)(g->b[tb - 1] + sw_u32(l + 12)) : (void *)0;
        memcpy(g->b[sw_u32(l)] + sw_u32(l + 4), &t, 8);
    }
    return args;
}

/* the object a parameter's word (blob + 1) << 32 | offset names, and a pointer into it */
static unsigned char *sw_blob(const SwG *g, const unsigned char *w8)
{
    unsigned long long w;
    memcpy(&w, w8, 8);
    return g->b[(w >> 32) - 1];
}

static void *sw_ptr(const SwG *g, const unsigned char *w8)
{
    unsigned long long w;
    memcpy(&w, w8, 8);
    return g->b[(w >> 32) - 1] + (unsigned)w;
}

/* a returned pointer as a tag: 0 NULL, (blob + 1) << 32 | offset, else ~0 */
static unsigned long long sw_tag(const SwG *g, void *r)
{
    unsigned i;
    unsigned long long t = r ? ~0ull : 0;
    for (i = 0; r && i < g->n; i++)
        if ((unsigned char *)r >= g->b[i] && (unsigned char *)r <= g->b[i] + g->size[i] + SW_SLACK)
            t = ((unsigned long long)(i + 1) << 32) | (unsigned)((unsigned char *)r - g->b[i]);
    return t;
}

static unsigned sw_out(SwG *g, unsigned char *out, unsigned o)
{
    unsigned i, base[64];
    for (i = 0; i < g->n; i++) {
        base[i] = o;
        memcpy(out + o, g->b[i], g->size[i]);
        o += g->size[i];
    }
    for (i = 0; i < g->nl; i++)
        memset(out + base[sw_u32(g->lk + 16 * i)] + sw_u32(g->lk + 16 * i + 4), 0, 8);
    for (i = 0; i < g->nz; i++)
        memset(out + base[sw_u32(g->zc + 8 * i)] + sw_u32(g->zc + 8 * i + 4), 0, 8);
    sw_alo = sw_ahi = 0;
    free(g->arena);
    free(g->b);
    free(g->size);
    return o;
}

typedef struct SwTh {
    const char *name;
    unsigned (*fn)(const unsigned char *, unsigned char *);
} SwTh;

static int sw_nib(int c)
{
    return c <= '9' ? c - '0' : c - 'a' + 10;
}

static int sw_main(const SwTh *tab, int ntab, const char *calls)
{
    static unsigned char in[1 << 18], out[1 << 17];
    char line[1 << 19 | 256];
    FILE *f = fopen(calls, "r");
#ifdef SW_FORK
    /* the results go through a private copy of stdout; what the program itself prints is discarded */
    FILE *res = fdopen(dup(1), "w");
    if (!res || !freopen("/dev/null", "w", stdout))
        return 2;
#endif
    if (!f)
        return 2;
    while (fgets(line, sizeof line, f)) {
        char *sp = strchr(line, ' '), *h;
        unsigned n = 0, i, k;
        int t;
        if (!sp)
            return 2;
        *sp = 0;
        for (t = 0; t < ntab && strcmp(tab[t].name, line); t++)
            ;
        if (t == ntab)
            return 3;
        h = sp + 1;
        if (*h != '-')
            for (; h[0] > ' ' && h[1] > ' '; h += 2)
                in[n++] = (unsigned char)(sw_nib(h[0]) << 4 | sw_nib(h[1]));
        memset(out, 0, sizeof out);
#ifdef SW_FORK
        /* library mode (EVAL_SWEEP_LIB): the call runs in a child, so a crash or a hang on generated
         * state is "!" for that call and the next one starts clean */
        {
            int status = 0;
            pid_t pid;
            fflush(res);
            pid = fork();
            if (pid == 0) {
                alarm(3);
                k = tab[t].fn(in, out);
                if (!k)
                    fputc(0x2d, res);
                for (i = 0; i < k; i++)
                    fprintf(res, "%02x", out[i]);
                fputc('\n', res);
                fflush(res);
                _exit(0);
            }
            if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status))
                fputs("!\n", res);
            fflush(res);
            continue;
        }
#endif
        k = tab[t].fn(in, out);
        if (!k)
            putchar('-');
        for (i = 0; i < k; i++)
            printf("%02x", out[i]);
        putchar('\n');
        fflush(stdout);
    }
    return 0;
}
