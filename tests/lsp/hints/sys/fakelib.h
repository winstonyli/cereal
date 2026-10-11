/* Stands in for a libc header (found through -isystem, so its declarations
 * are system ones: no parameter events, the text rule names the parameters). */
void *fake_copy(void *__restrict __dest, const void *__restrict __src,
                unsigned long __n);
unsigned long fake_len(const char *__s);
int fake_cmp(const char *, const char *);
int fake_cb(int (*__fn)(int __x), struct fake_s *__st, char __buf[static 4]);
