/* Pointer offsets the solver picks (docs/LAZY_BLOBS_DESIGN.md D11): two pointer parameters into one blob, and the guard
 * needs them 4 to 7 bytes apart, which the plain draws (whole 8-byte steps) never make; and two pointer cells of a
 * record that must point 3 bytes apart. */
struct ps_rec { const unsigned char *p, *q; };

int ps_win(const unsigned char *a, const unsigned char *l)
{
    return (a >= l - 7 && a < l - 3) ? 1 : 0;
}

int ps_cell(const struct ps_rec *r)
{
    return r->p && r->q && r->q - r->p == 3 ? 1 : 0;
}
