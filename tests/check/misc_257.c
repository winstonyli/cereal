// flags: -Wall
struct n { struct n *l; int c; };
#define COL(e) ((e)->c)
#define V1(t) do { struct n *o = (t)->l;       if (o)                                       COL(o) = 1;                              COL(t) = 2;                              } while (0)
#define V2(t) do { struct n *o = (t)->l;       if (o)                                       o->c = 1;                                t->c = 2;                                } while (0)
#define V3(t) do { struct n *o = (t)->l;       if (o)                                       o->c = 1;                                COL(t) = 2;                              } while (0)
