__attribute__ ((access (read_onl))) int f0 (char*);
__attribute__ ((access (1.5))) int f1 (char*);
__attribute__ ((access (read_only, 1))) int f2 (char*);
enum e1 { A = 256 } __attribute__((__mode__(__byte__)));
enum e2 { B = 1 } __attribute__((__mode__(__HI__)));
int sz[sizeof (enum e2) == 2 ? 1 : -1];
typedef struct __attribute__((mode(SI))) { int x; } S;
