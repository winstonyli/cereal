# a CRLF file keeps its line ends: the inserted #include line ends in CRLF,
# and the file is rewritten whole (nothing else changes)
. "$TESTS/fix/lib.sh"
printf 'int f(const char *s)\r\n{\r\n    puts(s);\r\n    return 0;\r\n}\r\n' >t/c.c
snap
run c.c
run c.c
