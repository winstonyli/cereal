// flags: -std=gnu99
/* A function named only in an unevaluated sizeof/typeof/alignof is not used
 * for "used but never defined" (a VLA one counts); a block-scope extern
 * inline declaration merges with the file-scope function. */

static void f0(void); 
void g0(void) { f0(); }


static void f1(void); 
void g1(void) { if (0) { f1(); } }


static int f2(void); 
void g2(void) { 0 ? f2() : 0; }


static int f3(void);
void g3(void) { sizeof(f3()); }


static int f4(void);
void g4(void) { sizeof(int (*)[f4()]); }


static int f5(void); 
void g5(void) { sizeof(int [0 ? f5() : 1]); }


static int f6(void);
void g6(void) { sizeof(sizeof(int [f6()])); }


static int h0(void); 
void k0(void) { __alignof__(h0()); }


static int h1(void);
void k1(void) { __typeof__(h1()) x; }


static int h2(void); 
void k2(void) { __typeof__(int [h2()]) x; }


static int h3(void); 
void k3(void) { __typeof__(int (*)[h3()]) x; }


static int h4(void);
void k4(void) { sizeof(__typeof__(int (*)[h3()])); }
void e1(void) {}
void e2(void) {}
static void test2(void)
{
  inline void e1(void);
  extern inline void e2(void);
  inline void e3(void);
  extern inline void e4(void);
}
void e3(void) {}
void e4(void) {}
