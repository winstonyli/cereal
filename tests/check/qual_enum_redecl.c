// flags: -std=gnu99
void bar (void);
volatile void bar () { }
volatile void baz (void);
void baz () { }
extern enum E e;
unsigned e;
extern enum E *p;
unsigned *p;
enum F { A } f;
unsigned f;
