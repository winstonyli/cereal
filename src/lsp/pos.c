/* pos.c - file URIs and LSP positions.
 *
 * LSP positions are (line, character) with characters counted in UTF-16
 * code units unless the client accepts UTF-8 (LSP 3.17 positionEncoding),
 * which is negotiated at initialize. */
#include "lsp.h"

#include <string.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

char *uri_to_path(Arena *a, const char *uri)
{
    StrBuf sb = {0};
    const char *p;
    char *r;
    if (strncmp(uri, "file://", 7) != 0)
        return NULL;
    p = uri + 7;
    if (*p != '/') { /* file://host/path: only localhost */
        if (strncmp(p, "localhost/", 10) != 0)
            return NULL;
        p += 9;
    }
    for (; *p; p++) {
        int h, l;
        if (*p == '%' && (h = hexval(p[1])) >= 0 && (l = hexval(p[2])) >= 0) {
            sb_putc(&sb, (char)(h * 16 + l));
            p += 2;
        } else {
            sb_putc(&sb, *p);
        }
    }
    r = path_normalize(a, sb_cstr(&sb));
    sb_free(&sb);
    return r;
}

char *path_to_uri(Arena *a, const char *path)
{
    static const char hex[] = "0123456789ABCDEF";
    StrBuf sb = {0};
    char *r;
    const unsigned char *p;
    sb_puts(&sb, "file://");
    for (p = (const unsigned char *)path; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || strchr("/-._~", *p)) {
            sb_putc(&sb, (char)*p);
        } else {
            sb_putc(&sb, '%');
            sb_putc(&sb, hex[*p >> 4]);
            sb_putc(&sb, hex[*p & 15]);
        }
    }
    r = arena_strndup(a, sb.data, sb.len);
    sb_free(&sb);
    return r;
}

/* UTF-8 sequence length from its lead byte (1 for invalid bytes). */
static size_t u8len(unsigned char c)
{
    return c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
}

size_t pos_to_offset(const char *text, size_t len, uint32_t line,
                     uint32_t character, PosEncoding enc)
{
    size_t off = 0;
    uint32_t units = 0;
    while (line && off < len) { /* \n, \r\n and \r end lines */
        char c = text[off++];
        if (c == '\n' || (c == '\r' && (off >= len || text[off] != '\n')))
            line--;
    }
    while (off < len && text[off] != '\n' && text[off] != '\r' &&
           units < character) {
        size_t n = u8len((unsigned char)text[off]);
        if (off + n > len)
            n = len - off;
        units += enc == ENC_UTF8 ? (uint32_t)n : n == 4 ? 2 : 1;
        off += n;
    }
    return off;
}

void loc_to_pos(SrcMgr *sm, SrcLoc loc, PosEncoding enc, uint32_t *line,
                uint32_t *character)
{
    SrcFile *f = srcmgr_file_of(sm, loc);
    uint32_t l, c, units = 0;
    const char *p, *end;
    if (!f) {
        *line = *character = 0;
        return;
    }
    srcmgr_linecol(f, loc, &l, &c);
    *line = l - 1;
    if (enc == ENC_UTF8) {
        *character = c - 1;
        return;
    }
    p = f->buf + (loc - f->base) - (c - 1);
    end = f->buf + (loc - f->base);
    while (p < end) {
        size_t n = u8len((unsigned char)*p);
        units += n == 4 ? 2 : 1;
        p += n;
    }
    *character = units;
}
