#define V 5
#if V >= 4
#endif
#ifdef A
#undef A
#endif
#ifdef A
#endif
#ifdef B
#define W 1
#else
#define W 2
#endif
#if W == 1
#endif
#if W >= 1
#endif
