/* parclient.h - how analyses and the index take part in a parallel run
 * (par.c). */
#ifndef CEREAL_PARCLIENT_H
#define CEREAL_PARCLIENT_H

#include "pp.h"
#include "thread.h"

/* A consumer of a parallel run besides the printer (an analysis, the
 * index).  Directive-driven events reach the listeners it attached to the
 * TU's preprocessor during phase A, live and in order.  Text-driven events
 * happen in the workers: fork gives each worker private state (attaching
 * listeners to the worker's PP; pp_event_key() tells which plan item an
 * event belongs to), token sees the worker's output tokens, and join
 * merges one worker's events with keys in [from, to) -- called in slice
 * order, so the merged result is the sequential one.  prepare, if given,
 * first runs for every slice concurrently (per-worker work that needs the
 * slice: filtering, sorting), so that join stays cheap.  Any hook may be
 * NULL; a client without fork only listens to phase A.
 *
 * With a cell cache (cell.h), encode turns a worker's events with keys in
 * [from, to) into a blob stored with the cell (in cenc_arena, locations
 * and definitions through cenc_*), and decode turns a reused cell's blob
 * back into worker state for wpp (a worker that does not run), which is
 * then joined like any other.  A client with fork but no encode makes the
 * cache unusable. */
struct CellEnc;
struct CellDec;

typedef struct ParClient {
    void *ctx;
    void *(*fork)(void *ctx, PP *wpp);
    void (*token)(void *wctx, const Tok *t);
    void (*prepare)(void *wctx, uint32_t from, uint32_t to);
    void (*join)(void *ctx, void *wctx, uint32_t from, uint32_t to);
    void (*finish)(void *ctx, ThreadPool *pool); /* pool may be NULL */
    void (*release)(void *ctx, void *wctx);
    void *(*encode)(void *ctx, void *wctx, uint32_t from, uint32_t to,
                    struct CellEnc *e);
    void *(*decode)(void *ctx, PP *wpp, const void *blob,
                    const struct CellDec *d);
} ParClient;

#endif
