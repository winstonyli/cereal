#include "inc/unterminated.h"
/* A comment that runs to the end of the file: a parallel run must still
 * lex it (and report it), and must not read past a segment while lexing
 * the text before it. */
int a;
   int indented_so_a_split_line_starts_with_blanks;
/* expect: error
int b;
int c;
