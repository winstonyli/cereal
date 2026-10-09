/* What -std= changes in the preprocessor: predefined macros, u/U/u8
 * literals, trigraphs, '::'. Run under every -std= by tests/run.sh. */
__STDC_VERSION__
#ifdef __STRICT_ANSI__
strict
#endif
#ifdef linux
linux
#endif
#ifdef unix
unix
#endif
#ifdef __STDC_UTF_16__
utf16
#endif
#ifdef __STDC_UTF_32__
utf32
#endif
u8"a" u"b" U"c" u'd' U'e'
t ??= ??( ??) ??< ??>
a::b
