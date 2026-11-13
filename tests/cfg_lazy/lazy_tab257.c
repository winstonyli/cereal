/* A read-only table of 257 entries (as Lua's luai_ctype_, UCHAR_MAX + 2) read at a loaded byte plus one: one byte value of 256 sets the
 * flag, so only a solver round that reads the whole table symbolically (SX_TAB, TABMAX) gets there; a table over the cap is read
 * concretely and the branch stays one-sided. */
static const unsigned char T[257] = { [0x38] = 8 };

int tb_run(const unsigned char *p)
{
    if (T[p[0] + 1] & 8)
        return 1;
    return 0;
}
