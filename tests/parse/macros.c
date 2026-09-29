/* Tokens made by the preprocessor (stringized, pasted, __LINE__) in one
 * function spanning many cells: parsed from regenerated cells, their
 * spellings must survive the cursors that made them. */
#define STR(x) #x
#define CAT(a, b) a##b
#define CHECK(c) ((c) ? 0 : fail(STR(c), __FILE__, __LINE__))
int fail(const char *what, const char *file, int line);
int CAT(check, _all)(int a, int b)
{
    int CAT(n, 1) = CHECK(a > 0);
    CHECK(b > 0);
    CHECK(a != b);
    CHECK(CAT(n, 1) == 0);
    return CHECK(a + b < 100) + CAT(0x, 1f);
}
