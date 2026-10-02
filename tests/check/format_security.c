// flags: -Wformat-security -Wformat-nonliteral
int printf(const char *, ...);
int fprintf(void *, const char *, ...);
char *g; char ga[4]; const char *cg; const char cga[] = "%s"; const char *const ccg = "x";
extern char *ext(void);

void f(char *s, const char *cs)
{
  char loc[10] = "x";
  char *lp = s;

  printf(s);
  printf(cs);
  printf(g);
  printf(ga);
  printf(cg);
  printf(ccg);
  printf(loc);
  printf(lp);
  printf(ext());
  printf(s + 1);
  printf(*&s);
  printf(1 ? s : cs);
  printf(1 ? "a" : "b");
  fprintf((void *)0, s);
  printf(s, 1);
  printf("%s", s);
}
void oneline(char *s){ printf(s); }
