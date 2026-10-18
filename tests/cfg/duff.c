/* Duff's device and other case labels nested inside inner statements:
   the T8 plan (section 3) cuts blocks by label, not by syntax. */
void duff(char *to, const char *from, int count)
{
    int n = (count + 7) / 8;
    switch (count % 8) {
    case 0: do { *to++ = *from++;
    case 7:      *to++ = *from++;
    case 6:      *to++ = *from++;
    case 5:      *to++ = *from++;
    case 4:      *to++ = *from++;
    case 3:      *to++ = *from++;
    case 2:      *to++ = *from++;
    case 1:      *to++ = *from++;
            } while (--n > 0);
    }
}

int nested(int x, int y)
{
    int r = 0;
    switch (x) {
    case 1:
        if (y) {
    case 2:
            r += 2;
        } else
            r += 3;
        break;
    default:
        for (;;) {
    case 3:
            r++;
            if (r > y) break;
        }
    }
    return r;
}
