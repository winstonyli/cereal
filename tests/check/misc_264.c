// flags: -std=gnu99
/* In the GNU modes "::" is one preprocessing token (libcpp CPP_SCOPE): pasting
 * onto it is invalid, a stray one is reported by spelling, and a spaced pair
 * of colons is two tokens.  [[ns::name]] still parses. */
#define CONCAT(a, b) a ## b
#define COLON2 : :
int x = 1 ? 2 : 3;
int y = 1 ? 2 COLON2 3;
CONCAT (::, >)
CONCAT (:, :)
[[gnu::unused]] static int z;
