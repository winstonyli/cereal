/* [[name(args)]]: a standard name keeps its arguments, any other is skipped as
 * balanced tokens and warned; the argument list makes std_attr_known run. */
[[unknown(1 +)]] int a;
[[__deprecated__("m")]] int b;
[[_Noreturn(1)]] void c(void);
[[nodiscard]] int d(void);
[[maybe_unused(2)]] int e;
