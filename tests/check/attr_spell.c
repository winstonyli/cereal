// flags: -std=gnu99 -Wall
/* Attribute names spelled with __ affixes are the same attributes; a name
 * wrapped twice is unknown.  __builtin_has_attribute reads the known set. */
int __attribute__((__nonnull__)) v;
void f(char *) __attribute__((__nonnull__));
int w __attribute__((__const));
int x __attribute__((____nonnull____));
void g(void);
int t1 = __builtin_has_attribute(g, __maybe_unused__);
int t2 = __builtin_has_attribute(g, nodiscard);
int t3 = __builtin_has_attribute(g, ____nonnull____);
int t4 = __builtin_has_attribute(g, __const__);
