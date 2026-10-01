int i;
void f(void){ --(int){1}; ++(int){2}; }
void g(void){ ++(typeof(i)){2}; }
void h(void){ ++(__typeof__(i)){2}; }
void k(void){ ++(int)i; --(char)i; }
