// flags: -std=gnu99 -Wall
/* gcc 15.2 warns on both attributes: the enumerator is not a function type. */
enum { A __attribute__((format(printf, 1, 2))), B __attribute__((nonnull)) };
