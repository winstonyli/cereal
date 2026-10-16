/* rpc.c - JSON-RPC framing over a byte stream (LSP base protocol). */
#include "lsp.h"

#include <string.h>
#include <strings.h>

static FILE *out_fp;
static Mutex out_m = PTHREAD_MUTEX_INITIALIZER;

void rpc_set_output(FILE *out)
{
    out_fp = out;
}

/* Headers are "Name: value\r\n" lines ending with an empty line; only
 * Content-Length matters. */
char *rpc_read(FILE *in, size_t *len)
{
    char line[1024];
    long long n = -1;
    char *body;
    size_t got;
    for (;;) {
        size_t k;
        if (!fgets(line, sizeof line, in))
            return NULL;
        k = strlen(line);
        while (k && (line[k - 1] == '\n' || line[k - 1] == '\r'))
            line[--k] = 0;
        if (k == 0) {
            if (n >= 0)
                break;
            continue; /* stray blank line before headers */
        }
        if (!strncasecmp(line, "Content-Length:", 15))
            n = atoll(line + 15);
    }
    body = xmalloc((size_t)n + 1);
    got = fread(body, 1, (size_t)n, in);
    if (got != (size_t)n) {
        free(body);
        return NULL;
    }
    body[n] = 0;
    *len = (size_t)n;
    return body;
}

void rpc_write(const char *body, size_t len)
{
    mutex_lock(&out_m);
    fprintf(out_fp, "Content-Length: %zu\r\n\r\n", len);
    fwrite(body, 1, len, out_fp);
    fflush(out_fp);
    mutex_unlock(&out_m);
}

/* ---- messages to the client ---------------------------------------------------- */

void rpc_begin(JsonWriter *w, StrBuf *sb, const char *method, const char *id)
{
    json_init_buf(w, sb);
    json_begin_object(w);
    json_key(w, "jsonrpc");
    json_str(w, "2.0");
    if (id) {
        json_key(w, "id");
        json_str(w, id);
    }
    json_key(w, "method");
    json_str(w, method);
    json_key(w, "params");
    json_begin_object(w);
}

void rpc_end(JsonWriter *w, StrBuf *sb)
{
    json_end_object(w);
    json_end_object(w);
    rpc_write(sb->data, sb->len);
    sb_free(sb);
}

void rpc_log(int type, const char *text)
{
    StrBuf sb = {0};
    JsonWriter w;
    rpc_begin(&w, &sb, "window/logMessage", NULL);
    json_key(&w, "type");
    json_int(&w, type);
    json_key(&w, "message");
    json_str(&w, text);
    rpc_end(&w, &sb);
}

void rpc_progress(const char *token, size_t token_len, const char *kind, const char *title,
                  const char *message, int pct)
{
    StrBuf sb = {0};
    JsonWriter w;
    if (!token)
        return;
    rpc_begin(&w, &sb, "$/progress", NULL);
    json_key(&w, "token");
    json_raw(&w, token, token_len);
    json_key(&w, "value");
    json_begin_object(&w);
    json_key(&w, "kind");
    json_str(&w, kind);
    if (title) {
        json_key(&w, "title");
        json_str(&w, title);
        json_key(&w, "cancellable");
        json_bool(&w, false);
    }
    if (message) {
        json_key(&w, "message");
        json_str(&w, message);
    }
    if (pct >= 0) {
        json_key(&w, "percentage");
        json_int(&w, pct);
    }
    json_end_object(&w);
    rpc_end(&w, &sb);
}
