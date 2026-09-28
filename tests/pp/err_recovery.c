/* Error recovery must match GCC's output, not just be diagnosed. */
#define F(a) [a]
#define G(a, b) [a b]
F(1, 2) after_too_many /* expect: error */
G(1) after_too_few /* expect: error */
F(
#pragma hoisted_before_result /* expect: directive-in-macro-args */
1) after_pragma
#line 40 not_a_string /* expect: error */
__LINE__
# 1 1 junk /* expect: error */
__LINE__
F( /* expect: error */
#pragma hoisted_before_name /* expect: directive-in-macro-args */
