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
