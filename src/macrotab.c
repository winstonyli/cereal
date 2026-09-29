/* macrotab.c - per-build macro state, indexed by identifier id. */
#include "macrotab.h"

const MacroSlot mt_empty_slot;

MacroTab *mt_new(void)
{
    return xcalloc(1, sizeof(MacroTab));
}

void mt_free(MacroTab *t)
{
    uint32_t p;
    if (!t)
        return;
    for (p = 0; p < ID_PAGES; p++)
        free(t->pages[p]);
    free(t);
}

MacroSlot *mt_slot_w(MacroTab *t, uint32_t id)
{
    uint32_t p = id >> ID_PAGE_BITS;
    if (p >= ID_PAGES)
        fatal("too many identifiers");
    if (!t->pages[p])
        t->pages[p] = xcalloc(ID_PAGE, sizeof(MacroSlot));
    return &t->pages[p][id & (ID_PAGE - 1)];
}
