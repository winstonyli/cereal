/* json.c - minimal streaming JSON writer. */
#include "json.h"

#include <string.h>

void json_init(JsonWriter *w, FILE *out)
{
    memset(w, 0, sizeof *w);
    w->out = out;
}

static void indent(JsonWriter *w)
{
    int i;
    if (!w->pretty)
        return;
    fputc('\n', w->out);
    for (i = 0; i < w->depth; i++)
        fputs("  ", w->out);
}

static void value_prefix(JsonWriter *w)
{
    if (w->after_key) {
        w->after_key = false;
        return;
    }
    if (w->depth > 0) {
        if (w->need_comma[w->depth])
            fputc(',', w->out);
        w->need_comma[w->depth] = true;
        indent(w);
    }
}

static void open_(JsonWriter *w, char c)
{
    value_prefix(w);
    fputc(c, w->out);
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
    fputc(c, w->out);
}

void json_begin_object(JsonWriter *w) { open_(w, '{'); }
void json_end_object(JsonWriter *w) { close_(w, '}'); }
void json_begin_array(JsonWriter *w) { open_(w, '['); }
void json_end_array(JsonWriter *w) { close_(w, ']'); }

static void raw_str(JsonWriter *w, const char *s, size_t n)
{
    size_t i;
    fputc('"', w->out);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': fputs("\\\"", w->out); break;
        case '\\': fputs("\\\\", w->out); break;
        case '\n': fputs("\\n", w->out); break;
        case '\r': fputs("\\r", w->out); break;
        case '\t': fputs("\\t", w->out); break;
        default:
            if (c < 0x20)
                fprintf(w->out, "\\u%04x", c);
            else
                fputc(c, w->out);
        }
    }
    fputc('"', w->out);
}

void json_key(JsonWriter *w, const char *k)
{
    value_prefix(w);
    raw_str(w, k, strlen(k));
    fputc(':', w->out);
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
    fprintf(w->out, "%lld", v);
}

void json_bool(JsonWriter *w, bool v)
{
    value_prefix(w);
    fputs(v ? "true" : "false", w->out);
}

void json_null(JsonWriter *w)
{
    value_prefix(w);
    fputs("null", w->out);
}
