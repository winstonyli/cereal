#ifndef API_H
#define API_H
struct point { int x, y; };
typedef struct point point_t;
enum color { RED, GREEN };
int area(point_t p);
extern int counter;
#define LIMIT 10
#endif
