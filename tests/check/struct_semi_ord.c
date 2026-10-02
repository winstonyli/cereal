// flags: -Wunused-variable -Wformat
struct s { struct { int a; } };
int *g(struct s *p) { return &p->a; }
