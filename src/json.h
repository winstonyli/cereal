/* json.h - minimal streaming JSON writer. */
#ifndef CEREAL_JSON_H
#define CEREAL_JSON_H

#include "common.h"

typedef struct JsonWriter {
    FILE *out;
    int depth;
    bool need_comma[64];
    bool after_key;
    bool pretty;
    size_t len;
    char buf[1 << 14];
} JsonWriter;

void json_init(JsonWriter *w, FILE *out);
void json_begin_object(JsonWriter *w);
void json_end_object(JsonWriter *w);
void json_begin_array(JsonWriter *w);
void json_end_array(JsonWriter *w);
void json_key(JsonWriter *w, const char *k);
void json_str(JsonWriter *w, const char *s);
void json_strn(JsonWriter *w, const char *s, size_t n);
void json_int(JsonWriter *w, long long v);
void json_bool(JsonWriter *w, bool v);
void json_null(JsonWriter *w);
/* Output is buffered and written when the top-level value closes. */

#endif
