// flags: -Wall
int g(void); int x, y;
void t(void) {
  (g(), 0);
  x >= y && (g(), 0);
  x >= y || (g(), 0);
  x >= y && (g(), 1);
  x >= y && (g(), y);
  x >= y && (g(), 0.0);
  x >= y && ((void)g(), 0);
  x >= y ? (g(), 0) : (g(), 0);
  x >= y && (g(), (0));
  x >= y && (g(), x - x + 2);
}
void u(void) {
  x >= y && y;
  x >= y || x + 1;
  x >= y && (x, y);
  (x >= y) && (g(), 1) ;
  x && y;
  x >= y && g();
}
