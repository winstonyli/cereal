# a UTF-8 byte order mark is kept and offsets count past it; BOM + CRLF too
. "$TESTS/fix/lib.sh"
printf '\357\273\277int f(const char *s)\n{\n    puts(s);\n    return 0;\n}\n' >t/b.c
printf '\357\273\277#define SUM 1 + 2\r\nint v = SUM;\r\nint f(const char *s)\r\n{\r\n    puts(s);\r\n    return v;\r\n}\r\n' >t/bc.c
snap
run b.c bc.c
run b.c bc.c
