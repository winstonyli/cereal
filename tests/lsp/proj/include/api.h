#ifndef API_H
#define API_H
#define API_VERSION 3
#define CAT(a, b) a ## b
#define GLUE CAT(API_, VERSION)
/* é: a two-byte character before the use (UTF-16 columns differ) */ int v = API_VERSION;
#endif
