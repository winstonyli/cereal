// flags: -Wnormalized=nfkc
#define P(a,b) a ## b
#define Q(a,b) int a ## b;
int x;
Q(
  À,
  ª
)
int P(ª,
 Ա) = 1;
int P(x, ª) = 2;
