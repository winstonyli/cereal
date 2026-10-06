// flags: -std=c99
/* gcc skips the rest of a bad [[ns::name(...)]] balancing ( [ and {, and
 * passes a ; inside it */
[[a::b({t})]] void f3();
[[a::b(;)]] int x;
[[a::b(1;2)]] int x2; int y = ;
[[a::b {t} ]] int x3;
[[c::d[{]]] int x4;
