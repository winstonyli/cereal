/* json.c - minimal streaming JSON writer. */
#include "json.h"

#include <string.h>

void json_init(JsonWriter *w, FILE *out)
{
    memset(w, 0, sizeof *w);
    w->out = out;
}

/* Buffered: stdio locks every call once threads exist, and JSON is
 * written a character at a time. */
static void flush_(JsonWriter *w)
{
    if (w->len) {
        fwrite(w->buf, 1, w->len, w->out);
        w->len = 0;
    }
}

static void put_(JsonWriter *w, char c)
{
    if (w->len == sizeof w->buf)
        flush_(w);
    w->buf[w->len++] = c;
}

static void puts_(JsonWriter *w, const char *s)
{
    while (*s)
        put_(w, *s++);
}

static void indent(JsonWriter *w)
{
    int i;
    if (!w->pretty)
        return;
    put_(w, '\n');
    for (i = 0; i < w->depth; i++)
        puts_(w, "  ");
}

static void value_prefix(JsonWriter *w)
{
    if (w->after_key) {
        w->after_key = false;
        return;
    }
    if (w->depth > 0) {
        if (w->need_comma[w->depth])
            put_(w, ',');
        w->need_comma[w->depth] = true;
        indent(w);
    }
}

static void open_(JsonWriter *w, char c)
{
    value_prefix(w);
    put_(w, c);
    if (w->depth < 63)
        w->depth++;
    w->need_comma[w->depth] = false;
}

static void close_(JsonWriter *w, char c)
{
    bool had = w->need_comma[w->depth];
    w->depth--;
    if (had)
        indent(w);
    put_(w, c);
    if (w->depth == 0)
        flush_(w); /* the document is complete */
}

void json_begin_object(JsonWriter *w) { open_(w, '{'); }
void json_end_object(JsonWriter *w) { close_(w, '}'); }
void json_begin_array(JsonWriter *w) { open_(w, '['); }
void json_end_array(JsonWriter *w) { close_(w, ']'); }

static void raw_str(JsonWriter *w, const char *s, size_t n)
{
    size_t i;
    put_(w, '"');
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': puts_(w, "\\\""); break;
        case '\\': puts_(w, "\\\\"); break;
        case '\n': puts_(w, "\\n"); break;
        case '\r': puts_(w, "\\r"); break;
        case '\t': puts_(w, "\\t"); break;
        default:
            if (c < 0x20)
            {
                char u[8];
                sprintf(u, "\\u%04x", c);
                puts_(w, u);
            }
            else
                put_(w, c);
        }
    }
    put_(w, '"');
}

void json_key(JsonWriter *w, const char *k)
{
    value_prefix(w);
    raw_str(w, k, strlen(k));
    put_(w, ':');
    w->after_key = true;
}

void json_str(JsonWriter *w, const char *s)
{
    json_strn(w, s, strlen(s));
}

void json_strn(JsonWriter *w, const char *s, size_t n)
{
    value_prefix(w);
    raw_str(w, s, n);
}

void json_int(JsonWriter *w, long long v)
{
    value_prefix(w);
    {
        char num[32];
        sprintf(num, "%lld", v);
        puts_(w, num);
    }
}

void json_bool(JsonWriter *w, bool v)
{
    value_prefix(w);
    puts_(w, v ? "true" : "false");
}

void json_null(JsonWriter *w)
{
    value_prefix(w);
    puts_(w, "null");
}
