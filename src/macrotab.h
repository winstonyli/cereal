/* macrotab.h - per-build macro state, indexed by identifier id.
 *
 * The interner holds only spellings, so it can outlive a build and be
 * shared by successive builds of a unit (ids stay stable across edits).
 * Everything a build learns about a name lives here instead.
 *
 * Writes happen in the sequential phase only (phase A, or a sequential
 * run); phase-B workers read after phase A has finished.  Pages are
 * allocated on first write; ids without a page read as the empty slot, so
 * workers may meet identifiers interned after phase A. */
#ifndef CEREAL_MACROTAB_H
#define CEREAL_MACROTAB_H

#include "intern.h"

typedef struct MacroSlot {
    struct Macro *cur;     /* definition in effect (sequential phase) */
    struct Macro *hist;    /* newest definition ever made (linked via prev) */
    uint32_t poison_seq;   /* poisoned from this version on; 0: never */
} MacroSlot;

typedef struct MacroTab {
    uint32_t npoison;      /* names ever poisoned (fast path for checks) */
    MacroSlot *pages[ID_PAGES];
} MacroTab;

extern const MacroSlot mt_empty_slot;

MacroTab *mt_new(void);
void mt_free(MacroTab *t);
/* The slot for writing (allocated on demand). */
MacroSlot *mt_slot_w(MacroTab *t, uint32_t id);

static inline const MacroSlot *mt_slot(const MacroTab *t, uint32_t id)
{
    const MacroSlot *pg = t->pages[id >> ID_PAGE_BITS];
    return pg ? &pg[id & (ID_PAGE - 1)] : &mt_empty_slot;
}

static inline struct Macro *mt_cur(const MacroTab *t, const Ident *id)
{
    return mt_slot(t, id->id)->cur;
}

static inline struct Macro *mt_hist(const MacroTab *t, const Ident *id)
{
    return mt_slot(t, id->id)->hist;
}

#endif
