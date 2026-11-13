/* A computed goto through a read-only table (CEREAL_TAB): 255 of the 256 entries are one label and the last is the handler
 * that matters, so the plain draws of p[0] almost never reach it, and only a solver round that reads the table symbolically
 * (SX_TAB) and asks which index goes to the unreached label gets there; the guard in the handler is then an ordinary branch. */
#define C4 &&common, &&common, &&common, &&common
#define C16 C4, C4, C4, C4
#define C64 C16, C16, C16, C16

int gt_run(const unsigned char *p)
{
    static const void *const tab[256] = { C64, C64, C64, C16, C16, C16, C4, C4, C4, &&common, &&common, &&common, &&rare };
    goto *tab[p[0]];
common:
    return 0;
rare:
    return p[1] == 0x5a ? 1 : 2;
}
