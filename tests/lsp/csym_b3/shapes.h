struct shape { int w, h; };
typedef int (*area_fn)(const struct shape *s, int scale);
extern area_fn area;
