/* json.c - minimal JSON: a streaming writer and a DOM reader. */
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
        if (w->sb)
            sb_putn(w->sb, w->buf, w->len);
        else
            fwrite(w->buf, 1, w->len, w->out);
        w->len = 0;
    }
}

void json_init_buf(JsonWriter *w, StrBuf *sb)
{
    memset(w, 0, sizeof *w);
    w->sb = sb;
}

void json_flush(JsonWriter *w)
{
    flush_(w);
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

void json_raw(JsonWriter *w, const char *text, size_t n)
{
    size_t i;
    value_prefix(w);
    for (i = 0; i < n; i++)
        put_(w, text[i]);
}

/* ---- reader --------------------------------------------------------- */

typedef struct JP {
    Arena *a;
    const char *p, *end;
    int depth;
} JP;

static void ws(JP *jp)
{
    while (jp->p < jp->end && (*jp->p == ' ' || *jp->p == '\t' ||
                               *jp->p == '\n' || *jp->p == '\r'))
        jp->p++;
}

static int hex4(const char *p)
{
    int v = 0, i;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= c - '0';
        else if (c >= 'a' && c <= 'f')
            v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            v |= c - 'A' + 10;
        else
            return -1;
    }
    return v;
}

static void put_utf8(StrBuf *sb, unsigned long cp)
{
    if (cp < 0x80) {
        sb_putc(sb, (char)cp);
    } else if (cp < 0x800) {
        sb_putc(sb, (char)(0xC0 | (cp >> 6)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        sb_putc(sb, (char)(0xE0 | (cp >> 12)));
        sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    } else {
        sb_putc(sb, (char)(0xF0 | (cp >> 18)));
        sb_putc(sb, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    }
}

static const char *parse_string(JP *jp, size_t *len)
{
    StrBuf sb = {0};
    const char *out;
    jp->p++; /* '"' */
    for (;;) {
        char c;
        if (jp->p >= jp->end)
            goto bad;
        c = *jp->p++;
        if (c == '"')
            break;
        if ((unsigned char)c < 0x20)
            goto bad;
        if (c != '\\') {
            sb_putc(&sb, c);
            continue;
        }
        if (jp->p >= jp->end)
            goto bad;
        c = *jp->p++;
        switch (c) {
        case '"': sb_putc(&sb, '"'); break;
        case '\\': sb_putc(&sb, '\\'); break;
        case '/': sb_putc(&sb, '/'); break;
        case 'b': sb_putc(&sb, '\b'); break;
        case 'f': sb_putc(&sb, '\f'); break;
        case 'n': sb_putc(&sb, '\n'); break;
        case 'r': sb_putc(&sb, '\r'); break;
        case 't': sb_putc(&sb, '\t'); break;
        case 'u': {
            int u, lo;
            unsigned long cp;
            if (jp->end - jp->p < 4 || (u = hex4(jp->p)) < 0)
                goto bad;
            jp->p += 4;
            cp = (unsigned long)u;
            if (u >= 0xD800 && u < 0xDC00 && jp->end - jp->p >= 6 &&
                jp->p[0] == '\\' && jp->p[1] == 'u' &&
                (lo = hex4(jp->p + 2)) >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + (((unsigned long)u - 0xD800) << 10) +
                     ((unsigned long)lo - 0xDC00);
                jp->p += 6;
            }
            put_utf8(&sb, cp);
            break;
        }
        default:
            goto bad;
        }
    }
    out = arena_strndup(jp->a, sb.data ? sb.data : "", sb.len);
    *len = sb.len;
    sb_free(&sb);
    return out;
bad:
    sb_free(&sb);
    return NULL;
}

static JsonValue *parse_value(JP *jp);

static JsonValue *parse_members(JP *jp, JsonValue *v, bool obj)
{
    VEC(JsonValue *) items = {0};
    VEC(const char *) keys = {0};
    char close = obj ? '}' : ']';
    jp->p++;
    ws(jp);
    if (jp->p < jp->end && *jp->p == close) {
        jp->p++;
        goto done;
    }
    for (;;) {
        JsonValue *m;
        ws(jp);
        if (obj) {
            size_t kl;
            const char *k;
            if (jp->p >= jp->end || *jp->p != '"' ||
                !(k = parse_string(jp, &kl)))
                goto bad;
            vec_push(&keys, k);
            ws(jp);
            if (jp->p >= jp->end || *jp->p != ':')
                goto bad;
            jp->p++;
        }
        if (!(m = parse_value(jp)))
            goto bad;
        vec_push(&items, m);
        ws(jp);
        if (jp->p < jp->end && *jp->p == ',') {
            jp->p++;
            continue;
        }
        if (jp->p < jp->end && *jp->p == close) {
            jp->p++;
            break;
        }
        goto bad;
    }
done:
    v->len = items.len;
    v->items = NEW_ARRAY(jp->a, JsonValue *, items.len + 1);
    if (items.len)
        memcpy(v->items, items.data, sizeof(JsonValue *) * items.len);
    if (obj) {
        v->keys = NEW_ARRAY(jp->a, const char *, keys.len + 1);
        if (keys.len)
            memcpy(v->keys, keys.data, sizeof(char *) * keys.len);
    }
    vec_free(&items);
    vec_free(&keys);
    return v;
bad:
    vec_free(&items);
    vec_free(&keys);
    return NULL;
}

static bool word(JP *jp, const char *w)
{
    size_t n = strlen(w);
    if ((size_t)(jp->end - jp->p) < n || memcmp(jp->p, w, n) != 0)
        return false;
    jp->p += n;
    return true;
}

static JsonValue *parse_value(JP *jp)
{
    JsonValue *v;
    const char *start;
    ws(jp);
    if (jp->p >= jp->end || ++jp->depth > 512)
        return NULL;
    v = NEW(jp->a, JsonValue);
    start = jp->p;
    switch (*jp->p) {
    case '{':
        v->kind = JV_OBJ;
        if (!parse_members(jp, v, true))
            return NULL;
        break;
    case '[':
        v->kind = JV_ARR;
        if (!parse_members(jp, v, false))
            return NULL;
        break;
    case '"':
        v->kind = JV_STR;
        if (!(v->str = parse_string(jp, &v->len)))
            return NULL;
        break;
    case 't':
        v->kind = JV_BOOL;
        v->b = true;
        if (!word(jp, "true"))
            return NULL;
        break;
    case 'f':
        v->kind = JV_BOOL;
        if (!word(jp, "false"))
            return NULL;
        break;
    case 'n':
        v->kind = JV_NULL;
        if (!word(jp, "null"))
            return NULL;
        break;
    default: {
        char buf[64];
        size_t n = 0;
        char *endp;
        while (jp->p < jp->end && n < sizeof buf - 1 &&
               (strchr("+-.eE", *jp->p) || (*jp->p >= '0' && *jp->p <= '9')))
            buf[n++] = *jp->p++;
        buf[n] = 0;
        if (!n)
            return NULL;
        v->kind = JV_NUM;
        v->num = strtod(buf, &endp);
        if (*endp)
            return NULL;
    }
    }
    v->text = start;
    v->text_len = (size_t)(jp->p - start);
    jp->depth--;
    return v;
}

JsonValue *json_parse(Arena *a, const char *s, size_t n)
{
    JP jp;
    JsonValue *v;
    jp.a = a;
    jp.p = s;
    jp.end = s + n;
    jp.depth = 0;
    v = parse_value(&jp);
    if (!v)
        return NULL;
    ws(&jp);
    return jp.p == jp.end ? v : NULL;
}

JsonValue *json_get(const JsonValue *obj, const char *key)
{
    size_t i;
    if (!obj || obj->kind != JV_OBJ)
        return NULL;
    for (i = 0; i < obj->len; i++)
        if (!strcmp(obj->keys[i], key))
            return obj->items[i];
    return NULL;
}

JsonValue *json_path(const JsonValue *obj, const char *path)
{
    char key[128];
    const JsonValue *v = obj;
    while (v && *path) {
        size_t n = strcspn(path, ".");
        if (n >= sizeof key)
            return NULL;
        memcpy(key, path, n);
        key[n] = 0;
        v = json_get(v, key);
        path += n + (path[n] == '.');
    }
    return (JsonValue *)v;
}

const char *json_str_of(const JsonValue *v, const char *dflt)
{
    return v && v->kind == JV_STR ? v->str : dflt;
}

long long json_int_of(const JsonValue *v, long long dflt)
{
    return v && v->kind == JV_NUM ? (long long)v->num : dflt;
}

bool json_bool_of(const JsonValue *v, bool dflt)
{
    return v && v->kind == JV_BOOL ? v->b : dflt;
}
