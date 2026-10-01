// flags: -std=gnu99
typedef unsigned short char16_t;
typedef unsigned int char32_t;
char16_t *a = u"ab";
char32_t b = U'c';
char *c = u8"z" "y";
int _Static_assert_dummy[sizeof(u"ab") == 6 ? 1 : -1];
#if u'a' == 97 && U'\xffffffff' > 0
int ok;
#endif
void *bad = u8"a" u"b";
