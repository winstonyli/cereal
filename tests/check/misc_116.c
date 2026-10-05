/* Errors of #if expressions: a string is "not valid"; a missing operand
 * names the operator before it (gcc's _cpp_parse_expr). */
#if 1 "x"
#endif
#if "x"
#endif
#if 1 ?
#endif
#if 1 ? 2
#endif
#if 1 ? 2 :
#endif
#if (1 ?)
#endif
#if ()
#endif
#if )
#endif
#if (
#endif
#if * 1
#endif
#if 1 + * 2
#endif
#if 1 ,
#endif
