/* plan.h - the phase-A stream: a TU's active text, split into segments
 * between directives (and at certified line starts), with the directives'
 * effects reduced to version markers.  See docs/PARALLEL.md. */
#ifndef CEREAL_PLAN_H
#define CEREAL_PLAN_H

#include "srcmgr.h"
#include "token.h"

struct IncludeFrame;

/* Immutable snapshot of an include frame (a #line creates a new one). */
typedef struct PlanFrame {
    struct PlanFrame *parent;
    SrcFile *file;
    const char *presumed_name;
    const LineAdj *adj;
    SrcLoc include_loc;
    int dir_index;
    int depth;                 /* include depth (main file = 1) */
    bool system;               /* system header as of this snapshot */
} PlanFrame;

typedef enum {
    PI_SEG,                    /* active text [begin, end) */
    PI_DIR,                    /* a directive line was here */
    PI_ENTER,                  /* entering frame's file */
    PI_EXIT,                   /* leaving the current file */
    PI_PRAGMA                  /* a #pragma for the output stream */
} PlanKind;

typedef struct PlanItem {
    uint8_t kind;
    bool split;                /* SEG: begins at a line start, not a directive */
    bool cut;                  /* SEG: a content-defined cell boundary (cdc) */
    uint32_t version;          /* macro version in effect after this item */
    uint32_t counter;          /* __COUNTER__ value after this item */
    SrcLoc begin, end;         /* PI_PRAGMA: end indexes Plan.pragmas */
    PlanFrame *frame;          /* frame in effect after this item */
} PlanItem;

typedef struct Plan {
    VEC(PlanItem) items;
    VEC(Tok) pragmas;          /* TK_PRAGMA tokens of PI_PRAGMA items */
    uint64_t text_bytes;       /* total segment bytes */
    size_t chunk;              /* max segment size before a split */
    /* Content-defined splitting (for the cell cache): split where the text
     * says so rather than every `chunk` bytes, so that an edit moves only
     * the boundaries next to it.  Averages about `chunk` bytes. */
    bool cdc;
    uint64_t cdc_bytes;        /* text since the last cut */
    bool cdc_fire;             /* a cut is due at the next line start */
} Plan;

void plan_free(Plan *p);

#endif
