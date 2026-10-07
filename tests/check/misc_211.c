// flags: -DD=1
#define A
#define A 1
#define str(x) #x
#define str(x) %: x
#undef str
#define cat3(x,y,z) x##y####z
#define cat3(x,y,z) x####y##z
#undef cat3
#define cat3(x,y,z) x##y####z
#define cat3(x,y,z) x##y####z
#define D 1 2
