/* json.h - minimal JSON: a streaming writer and a DOM reader. */
#ifndef CEREAL_JSON_H
#define CEREAL_JSON_H

#include "common.h"

typedef struct JsonWriter {
    FILE *out;
    StrBuf *sb;              /* instead of out: write into memory */
    int depth;
    bool need_comma[64];
    bool after_key;
    bool pretty;
    size_t len;
    char buf[1 << 14];
} JsonWriter;

void json_init(JsonWriter *w, FILE *out);
void json_init_buf(JsonWriter *w, StrBuf *sb);
void json_flush(JsonWriter *w);
/* A value that is already JSON text (e.g. a request id echoed back). */
void json_raw(JsonWriter *w, const char *text, size_t n);
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

/* ---- reader --------------------------------------------------------- */

typedef enum { JV_NULL, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_OBJ } JsonKind;

typedef struct JsonValue {
    JsonKind kind;
    bool b;
    double num;
    const char *str;         /* JV_STR: unescaped, NUL-terminated */
    size_t len;              /* JV_STR: bytes; JV_ARR/JV_OBJ: members */
    struct JsonValue **items;
    const char **keys;       /* JV_OBJ */
    const char *text;        /* the value's source text ... */
    size_t text_len;         /* ... (to echo ids verbatim) */
} JsonValue;

/* Parse one JSON document into arena memory; NULL on a syntax error. */
JsonValue *json_parse(Arena *a, const char *s, size_t n);
JsonValue *json_get(const JsonValue *obj, const char *key);
/* obj.a.b.c ("a.b.c"); NULL if any step is missing */
JsonValue *json_path(const JsonValue *obj, const char *path);
const char *json_str_of(const JsonValue *v, const char *dflt);
long long json_int_of(const JsonValue *v, long long dflt);
bool json_bool_of(const JsonValue *v, bool dflt);

#endif
