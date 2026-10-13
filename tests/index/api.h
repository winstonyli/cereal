#ifndef API_H
#define API_H
#define VERSION 3
#define SQ(x) ((x) * (x))
#define CALL_TWICE(v) (helper(v) + helper(v))
struct pt { int x, y; };
typedef struct pt pt_t;
enum color { RED, GREEN = 4 };
extern int shared_counter;
int helper(int);
int declared_only(int);
static inline int twice(int v) { return v * 2; }
#endif
