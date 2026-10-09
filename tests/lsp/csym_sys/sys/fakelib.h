/* Stands in for a libc header: found through -isystem, so its
 * declarations are system ones. */
unsigned long fake_len(const char *s);
int fake_printf(const char *restrict fmt, ...);
