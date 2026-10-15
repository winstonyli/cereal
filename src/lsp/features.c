/* features.c - LSP requests answered from a snapshot's index.
 *
 * Macros get the features variables and functions get elsewhere:
 * definition, references, hover, completion, document symbols, semantic
 * tokens, folding, rename, call hierarchy and signature help, plus
 * diagnostics, inactive regions and a macro expansion view. */
#include "lsp.h"

#include "../c/frontend.h"
#include "projidx.h"
#include "project.h"

#include <ctype.h>
#include <string.h>

/* ---- helpers ------------------------------------------------------------ */

SrcLoc req_loc_in(Req *r, SrcFile *file, const JsonValue *pos)
{
    uint32_t line = (uint32_t)json_int_of(json_get(pos, "line"), 0);
    SrcLoc ls = line ? srcmgr_loc_of(file, line + 1, 1) : 0;
    size_t base = !line ? 0 : ls ? ls - file->base : file->size;
    size_t off = base + pos_to_offset(file->buf + base, file->size - base, 0,
                                      (uint32_t)json_int_of(json_get(pos,
                                                                     "character"),
                                                            0),
                                      r->enc);
    return file->base + (SrcLoc)off;
}

SrcLoc req_loc(Req *r, const JsonValue *pos)
{
    return req_loc_in(r, r->file, pos);
}

static SrcLoc cursor(Req *r)
{
    return req_loc(r, json_get(r->params, "position"));
}

static void json_pos(JsonWriter *w, SrcMgr *sm, PosEncoding enc, SrcLoc loc)
{
    uint32_t l, c;
    loc_to_pos(sm, loc, enc, &l, &c);
    json_begin_object(w);
    json_key(w, "line");
    json_int(w, l);
    json_key(w, "character");
    json_int(w, c);
    json_end_object(w);
}

void json_range(JsonWriter *w, SrcMgr *sm, PosEncoding enc, SrcLoc b, SrcLoc e)
{
    json_begin_object(w);
    json_key(w, "start");
    json_pos(w, sm, enc, b);
    json_key(w, "end");
    json_pos(w, sm, enc, e < b ? b : e);
    json_end_object(w);
}

static const char *uri_of(Arena *a, SrcMgr *sm, SrcLoc loc)
{
    SrcFile *f = srcmgr_file_of(sm, loc);
    return f ? path_to_uri(a, f->path) : NULL;
}

void json_location(JsonWriter *w, Req *r, SrcLoc b, SrcLoc e)
{
    const SrcFile *f = srcmgr_file_of(&r->snap->tu.sm, b);
    if (!f || f != r->uri_file) { /* results come in runs of one file */
        r->uri_file = f;
        r->uri = uri_of(r->arena, &r->snap->tu.sm, b);
    }
    json_begin_object(w);
    json_key(w, "uri");
    json_str(w, r->uri);
    json_key(w, "range");
    json_range(w, &r->snap->tu.sm, r->enc, b, e);
    json_end_object(w);
}

/* A location worth showing: in a real file (not <built-in>, not scratch). */
static bool real_loc(SrcMgr *sm, SrcLoc loc)
{
    SrcFile *f = loc ? srcmgr_file_of(sm, loc) : NULL;
    return f && (f->kind == SF_USER || f->kind == SF_SYSTEM);
}

static bool in_file(const SrcFile *f, SrcLoc loc)
{
    return loc >= f->base && loc <= f->base + f->size;
}

static const char *signature(Req *r, Macro *m)
{
    return macro_signature(r->arena, m);
}

/* ---- definition, references, hover --------------------------------------- */

/* ---- C symbols (docs/B2_DESIGN.md section 6) ------------------------------ */

enum { DH_READ = 2, DH_WRITE = 3 }; /* LSP DocumentHighlightKind */

/* The C entities named at loc in the request's file, from the check's index,
 * when the macro index's answer t does not stand: nothing, or a name it only
 * knows by its plain identifier (weak).  None if there is no index yet, or
 * the file's text differs. */
static size_t c_decls_at(Req *r, const IdxTarget *t, SrcLoc loc, uint32_t *out,
                         size_t max)
{
    if ((t->kind != TGT_NONE && !t->weak) || !r->cidx || loc < r->file->base)
        return 0;
    return cindex_decls_at(r->cidx, r->file->path, loc - r->file->base, out, max);
}

/* The events q selects for the decls, as locations, or with highlight as
 * DocumentHighlights (Write for a declaration or definition, Read for a
 * use) in the request's file only.  A file the snapshot lacks (the phases
 * saw different include sets) has no line table here: left out. */
static void c_events(Req *r, JsonWriter *w, const uint32_t *decls, size_t nd,
                     CIdxQuery q, bool highlight)
{
    const CIndex *ix = r->cidx;
    int fi = highlight ? cindex_file(ix, r->file->path) : -1;
    uint32_t *ev, last = UINT32_MAX;
    SrcFile *f = NULL;
    size_t n, i;
    if (highlight && fi < 0)
        return;
    n = cindex_select(ix, decls, nd, q, fi, &ev);
    for (i = 0; i < n; i++) {
        const CIdxEvent *e = &ix->ev[ev[i]];
        SrcLoc b;
        if (e->file != last) {
            last = e->file;
            f = cindex_srcfile(&r->snap->tu.sm, ix->files[e->file].path);
        }
        if (!f)
            continue;
        b = f->base + e->off;
        if (!highlight) {
            json_location(w, r, b, b + e->len);
            continue;
        }
        json_begin_object(w);
        json_key(w, "range");
        json_range(w, &r->snap->tu.sm, r->enc, b, b + e->len);
        json_key(w, "kind");
        json_int(w, (e->flags & CIX_ROLE) == CIX_REF ? DH_READ : DH_WRITE);
        json_end_object(w);
    }
    free(ev);
}

/* definition and declaration: a macro expanded here answers; a C entity
 * answers over a name with only macro history (weak) or none. */
static void goto_entity(Req *r, JsonWriter *w, CIdxQuery q)
{
    SrcLoc at = cursor(r);
    IdxTarget t = index_resolve(&r->snap->ix, at);
    SrcMgr *sm = &r->snap->tu.sm;
    uint32_t decls[16];
    size_t nd = c_decls_at(r, &t, at, decls, 16);
    int k;
    json_begin_array(w);
    if (nd) {
        c_events(r, w, decls, nd, q, false);
    } else if (t.kind == TGT_MACRO) {
        for (k = 0; k < t.nmacros; k++)
            if (real_loc(sm, t.macros[k]->name_loc))
                json_location(w, r, t.macros[k]->name_loc,
                              t.macros[k]->name_loc + t.macros[k]->name->len);
    } else if (t.kind == TGT_PARAM) {
        Macro *m = t.macros[0];
        json_location(w, r, m->param_locs[t.param],
                      m->param_locs[t.param] + m->params[t.param]->len);
    } else if (t.kind == TGT_INCLUDE && t.file) {
        json_begin_object(w);
        json_key(w, "uri");
        json_str(w, path_to_uri(r->arena, t.file->path));
        json_key(w, "range");
        json_range(w, sm, r->enc, t.file->base, t.file->base);
        json_end_object(w);
    }
    json_end_array(w);
}

void lsp_definition(Req *r, JsonWriter *w)
{
    goto_entity(r, w, CIQ_DEF);
}

void lsp_declaration(Req *r, JsonWriter *w)
{
    goto_entity(r, w, CIQ_DECL);
}

/* typeDefinition: the definitions of the types of the C entities at the
 * cursor (their typedef, struct, union or enum; B3_DESIGN.md 5.1). */
void lsp_type_definition(Req *r, JsonWriter *w)
{
    SrcLoc at = cursor(r);
    IdxTarget t = index_resolve(&r->snap->ix, at);
    uint32_t decls[16], types[16];
    size_t nd = c_decls_at(r, &t, at, decls, 16);
    json_begin_array(w);
    if (nd && (nd = cindex_types(r->cidx, decls, nd, types)))
        c_events(r, w, types, nd, CIQ_DEF, false);
    json_end_array(w);
}

/* references and documentHighlight (only the request's file): the macro
 * index's uses of a macro or macro parameter (its definitions are the
 * declarations, Write), else the C index's events of the entity. */
static void uses(Req *r, JsonWriter *w, bool highlight)
{
    SrcLoc at = cursor(r);
    IdxTarget t = index_resolve(&r->snap->ix, at);
    uint32_t decls[16];
    size_t nd = c_decls_at(r, &t, at, decls, 16);
    bool decl = highlight ||
                json_bool_of(json_path(r->params, "context.includeDeclaration"),
                             true);
    json_begin_array(w);
    if (nd) {
        c_events(r, w, decls, nd, decl ? CIQ_REFS : CIQ_USES, highlight);
    } else if (t.kind == TGT_MACRO || t.kind == TGT_PARAM) {
        IdxRef *refs;
        size_t n = index_references(&r->snap->ix, &t, &refs), i;
        size_t ndef = t.kind == TGT_PARAM ? 1 : (size_t)t.nmacros;
        for (i = 0; i < n; i++) {
            SrcLoc b = refs[i].loc;
            if ((i < ndef && !decl) || !real_loc(&r->snap->tu.sm, b) ||
                (highlight && !in_file(r->file, b)))
                continue;
            if (!highlight) {
                json_location(w, r, b, b + refs[i].len);
                continue;
            }
            json_begin_object(w);
            json_key(w, "range");
            json_range(w, &r->snap->tu.sm, r->enc, b, b + refs[i].len);
            json_key(w, "kind");
            json_int(w, i < ndef ? DH_WRITE : DH_READ);
            json_end_object(w);
        }
    }
    json_end_array(w);
}

void lsp_references(Req *r, JsonWriter *w)
{
    uses(r, w, false);
}

void lsp_document_highlight(Req *r, JsonWriter *w)
{
    uses(r, w, true);
}

/* A decl of a carried index whose declaration was edited (B3_DESIGN.md 12.7):
 * a DECL or DEF event lost to the edit (cindex_touched), or kept on a line the
 * damage touches (`int x` changed to `long x` keeps x's event). */
static bool c_edited(Req *r, const uint32_t *decls, size_t nd)
{
    const CIndex *ix = r->cidx;
    size_t i;
    uint32_t j;
    for (i = 0; i < nd; i++) {
        if (cindex_touched(ix, decls[i]))
            return true;
        for (j = ix->by_decl_start[decls[i]];
             j < ix->by_decl_start[decls[i] + 1]; j++) {
            const CIdxEvent *e = &ix->ev[ix->by_decl[j]];
            const CIdxFile *cf = &ix->files[e->file];
            SrcFile *f;
            uint32_t l, l1, l2, c;
            if ((e->flags & CIX_ROLE) == CIX_REF || !cf->edited ||
                !(f = cindex_srcfile(&r->snap->tu.sm, cf->path)))
                continue;
            srcmgr_linecol(f, f->base + e->off, &l, &c);
            srcmgr_linecol(f, f->base + cf->dmg_begin, &l1, &c);
            /* the damage is inclusive: its end at a line start touches
             * nothing of that line */
            srcmgr_linecol(f, f->base + (cf->dmg_end > cf->dmg_begin
                                             ? cf->dmg_end - 1 : cf->dmg_end),
                           &l2, &c);
            if (l >= l1 && l <= l2)
                return true;
        }
    }
    return false;
}

/* hover: a macro expanded here answers as before; a C entity answers over a
 * name with only macro history (weak, which it then mentions) or none: the
 * checker's text for it (cindex_hover), over the name.  Elsewhere in a
 * skipped #if group, a note saying so (rather than nothing). */
void lsp_hover(Req *r, JsonWriter *w)
{
    SrcLoc at = cursor(r);
    IdxTarget t = index_resolve(&r->snap->ix, at);
    StrBuf sb = {0};
    uint32_t decls[16], first;
    size_t nd = c_decls_at(r, &t, at, decls, 16);
    int k;
    if (nd) {
        const CIdxEvent *e;
        cindex_hover(r->cidx, decls, nd, true, &sb);
        if (t.weak)
            sb_puts(&sb, "\n\n(also a macro name)");
        if (r->c_carried && c_edited(r, decls, nd))
            sb_puts(&sb, "\n\n(rechecking: its declaration was edited)");
        cindex_at(r->cidx, (uint32_t)cindex_file(r->cidx, r->file->path),
                  at - r->file->base, &first);
        e = &r->cidx->ev[first];
        t.range.begin = r->file->base + e->off;
        t.range.end = t.range.begin + e->len;
    } else if (t.kind == TGT_NONE) {
        if (!index_inactive_note(&r->snap->ix, at, &sb)) {
            json_null(w);
            return;
        }
    } else if (t.kind == TGT_PARAM) {
        sb_printf(&sb, "parameter `%s` of `%s`", t.name->str,
                  signature(r, t.macros[0]));
    } else if (t.kind == TGT_INCLUDE) {
        sb_printf(&sb, "`%s`", t.file ? t.file->path : "(not found)");
    } else {
        for (k = 0; k < t.nmacros; k++) {
            Macro *m = t.macros[k];
            SrcFile *f = srcmgr_file_of(&r->snap->tu.sm, m->name_loc);
            uint32_t l = 0, c = 0;
            if (f)
                srcmgr_linecol(f, m->name_loc, &l, &c);
            sb_printf(&sb, "%s```c\n#define %s %s\n```\n", k ? "\n---\n" : "",
                      signature(r, m), macro_body_str(&r->snap->tu.pp, m));
            if (f)
                sb_printf(&sb, "defined at %s:%u", f->path, l);
            if (m->undef_loc)
                sb_puts(&sb, " (later #undef)");
            sb_putc(&sb, '\n');
        }
        if (!t.nmacros)
            sb_printf(&sb, "`%s` is not a macro here", t.name->str);
        if (t.top && t.top->text.len)
            sb_printf(&sb, "\nexpands to:\n```c\n%s\n```", sb_cstr(&t.top->text));
    }
    json_begin_object(w);
    json_key(w, "contents");
    json_begin_object(w);
    json_key(w, "kind");
    json_str(w, "markdown");
    json_key(w, "value");
    json_str(w, sb_cstr(&sb));
    json_end_object(w);
    if (t.range.end) {
        json_key(w, "range");
        json_range(w, &r->snap->tu.sm, r->enc, t.range.begin, t.range.end);
    }
    json_end_object(w);
    sb_free(&sb);
}

/* ---- completion, symbols ------------------------------------------------- */

enum { CK_FUNCTION = 3, CK_CONSTANT = 21 };   /* CompletionItemKind */
enum { SK_FUNCTION = 12, SK_CONSTANT = 14 };  /* SymbolKind */

/* The semantic token legend: macro tokens first, then C names by kind. */
enum {
    ST_MACRO, ST_PARAM, ST_FUNCTION, ST_VARIABLE, ST_TYPE, ST_ENUMMEMBER,
    ST_PROPERTY, ST_STRUCT, ST_ENUM
};
const char *const lsp_token_types[] = {
    "macro", "parameter", "function", "variable", "type", "enumMember",
    "property", "struct", "enum", NULL};
enum { SM_DECLARATION = 1, SM_READONLY = 2, SM_STATIC = 4 };
const char *const lsp_token_modifiers[] = {"declaration", "readonly", "static",
                                           NULL};

/* LSP kinds of a C entity by CIdxKind: completion item, symbol, semantic
 * token type (-1: none; labels). */
static const struct {
    int8_t ck, sk, st;
} C_KINDS[] = {
    [CIK_FUNC] = {3, 12, ST_FUNCTION},
    [CIK_OBJ] = {6, 13, ST_VARIABLE},
    [CIK_PARAM] = {6, 13, ST_PARAM},
    [CIK_TYPEDEF] = {7, 5, ST_TYPE},
    [CIK_ENUMCONST] = {20, 22, ST_ENUMMEMBER},
    [CIK_FIELD] = {5, 8, ST_PROPERTY},
    [CIK_STRUCT] = {22, 23, ST_STRUCT},
    [CIK_UNION] = {22, 23, ST_STRUCT},
    [CIK_ENUM] = {13, 10, ST_ENUM},
    [CIK_LABEL] = {0, 0, -1}};

/* The index's file entry for the request's document, -1 if none or stale. */
static int c_file(Req *r)
{
    int fi = r->cidx ? cindex_file(r->cidx, r->file->path) : -1;
    return fi >= 0 && !r->cidx->files[fi].stale ? fi : -1;
}

/* The first line of a decl's hover text. */
static const char *c_detail(Req *r, uint32_t d)
{
    const char *h = r->cidx->strings + r->cidx->decls[d].hover;
    const char *nl = strchr(h, '\n');
    return nl ? arena_strndup(r->arena, h, (size_t)(nl - h)) : h;
}

/* completion: the macros visible at the cursor, then the C names visible
 * there that no visible macro hides (B3_DESIGN.md 5.5). */
void lsp_completion(Req *r, JsonWriter *w)
{
    Macro **v;
    SrcLoc at = cursor(r);
    size_t n = index_visible(&r->snap->ix, at, &v), i, nc = 0;
    uint32_t *cv = NULL, seq = index_seq_at(&r->snap->ix, at);
    if (c_file(r) >= 0)
        nc = cindex_visible(r->cidx, r->file->path, at - r->file->base, &cv);
    json_begin_object(w);
    json_key(w, "isIncomplete");
    json_bool(w, r->c_carried); /* ask again: the check will add the damage */
    json_key(w, "items");
    json_begin_array(w);
    for (i = 0; i < n; i++) {
        Macro *m = v[i];
        json_begin_object(w);
        json_key(w, "label");
        json_str(w, m->name->str);
        json_key(w, "kind");
        json_int(w, m->funclike ? CK_FUNCTION : CK_CONSTANT);
        json_key(w, "detail");
        json_str(w, arena_printf(r->arena, "#define %s %s", signature(r, m),
                                 macro_body_str(&r->snap->tu.pp, m)));
        json_end_object(w);
    }
    for (i = 0; i < nc; i++) {
        const char *name = cindex_name(r->cidx, cv[i]);
        Ident *id = intern_find(r->snap->tu.pp.in, name, strlen(name));
        if (id && macro_at_version(r->snap->tu.pp.mt, id, seq))
            continue;        /* a macro hides it */
        json_begin_object(w);
        json_key(w, "label");
        json_str(w, name);
        json_key(w, "kind");
        json_int(w, C_KINDS[r->cidx->decls[cv[i]].kind].ck);
        json_key(w, "detail");
        json_str(w, r->cidx->strings + r->cidx->decls[cv[i]].hover);
        json_end_object(w);
    }
    free(cv);
    json_end_array(w);
    json_end_object(w);
}

/* A document symbol of the C index: an entry (file-level declaration) or a
 * member (field, enumerator) under the node that claims its parent. */
typedef struct CSymNode {
    uint32_t ev, decl;
    uint32_t b, e;           /* range, offsets in the file */
    int32_t up;              /* the claiming node, -1: top level */
    int32_t kid, next;       /* its first child, its next sibling; -1 */
} CSymNode;

static void c_symbol(Req *r, JsonWriter *w, const CSymNode *nodes, int32_t k,
                     int depth)
{
    const CIndex *ix = r->cidx;
    const CIdxEvent *e = &ix->ev[nodes[k].ev];
    SrcLoc base = r->file->base;
    int32_t j;
    json_begin_object(w);
    json_key(w, "name");
    json_str(w, cindex_name(ix, nodes[k].decl));
    json_key(w, "detail");
    json_str(w, c_detail(r, nodes[k].decl));
    json_key(w, "kind");
    json_int(w, C_KINDS[ix->decls[nodes[k].decl].kind].sk);
    json_key(w, "range");
    json_range(w, &r->snap->tu.sm, r->enc, base + nodes[k].b, base + nodes[k].e);
    json_key(w, "selectionRange");
    json_range(w, &r->snap->tu.sm, r->enc, base + e->off, base + e->off + e->len);
    if (nodes[k].kid >= 0 && depth < 8) {
        json_key(w, "children");
        json_begin_array(w);
        for (j = nodes[k].kid; j >= 0; j = nodes[j].next)
            c_symbol(r, w, nodes, j, depth + 1);
        json_end_array(w);
    }
    json_end_object(w);
}

/* The C part of documentSymbol (B3_DESIGN.md 5.2): file-level declarations
 * with the external declaration as range, fields and enumerators under the
 * node whose type is their record or enum (a definition of it first). */
static void c_document_symbols(Req *r, JsonWriter *w)
{
    const CIndex *ix = r->cidx;
    int fi = c_file(r);
    VEC(CSymNode) v = {0};
    uint32_t i, end, *claim;
    size_t k;
    if (fi < 0)
        return;
    end = cindex_lower(ix, (uint32_t)fi + 1, 0);
    for (i = cindex_lower(ix, (uint32_t)fi, 0); i < end; i++) {
        const CIdxEvent *e = &ix->ev[i];
        const CIdxDecl *x = &ix->decls[e->decl];
        uint32_t s;
        CSymNode n;
        bool member = x->kind == CIK_FIELD || x->kind == CIK_ENUMCONST;
        if (!cindex_symbol_event(ix, i, &s))
            continue;
        n.ev = i;
        n.decl = e->decl;
        n.b = e->off;
        n.e = e->off + e->len;
        n.up = n.kid = n.next = -1;
        if (s && !member) {                  /* the external declaration */
            n.b = ix->scopes[s - 1].begin < n.b ? ix->scopes[s - 1].begin : n.b;
            n.e = ix->scopes[s - 1].end > n.e ? ix->scopes[s - 1].end : n.e;
        }
        vec_push(&v, n);
    }
    /* claim: decl -> 1 + the node holding its members */
    claim = xcalloc(ix->ndecls + 1, sizeof *claim);
    for (k = 0; k < v.len; k++) {            /* a definition of the type */
        const CIdxDecl *x = &ix->decls[v.data[k].decl];
        if (x->type == v.data[k].decl + 1 && !claim[v.data[k].decl] &&
            (ix->ev[v.data[k].ev].flags & CIX_ROLE) == CIX_DEF)
            claim[v.data[k].decl] = (uint32_t)k + 1;
    }
    for (k = 0; k < v.len; k++) {            /* else the first entry of it */
        const CIdxDecl *x = &ix->decls[v.data[k].decl];
        uint32_t t = x->type;
        if (t && !claim[t - 1] && x->kind != CIK_FIELD &&
            x->kind != CIK_ENUMCONST)
            claim[t - 1] = (uint32_t)k + 1;
    }
    for (k = v.len; k-- > 0;) {              /* children lists, in order */
        const CIdxDecl *x = &ix->decls[v.data[k].decl];
        uint32_t c = x->parent ? claim[x->parent - 1] : 0;
        if (c && c - 1 != k) {
            v.data[k].up = (int32_t)(c - 1);
            v.data[k].next = v.data[c - 1].kid;
            v.data[c - 1].kid = (int32_t)k;
        }
    }
    free(claim);
    for (k = 0; k < v.len; k++)
        if (v.data[k].up < 0 && ix->decls[v.data[k].decl].kind != CIK_FIELD)
            c_symbol(r, w, v.data, (int32_t)k, 0);
    vec_free(&v);
}

void lsp_document_symbols(Req *r, JsonWriter *w)
{
    PP *pp = &r->snap->tu.pp;
    size_t i;
    json_begin_array(w);
    for (i = 0; i < pp->macros.len; i++) {
        Macro *m = pp->macros.data[i];
        if (m->alias_of || !in_file(r->file, m->name_loc) || !m->name_loc)
            continue;
        json_begin_object(w);
        json_key(w, "name");
        json_str(w, m->name->str);
        json_key(w, "detail");
        json_str(w, signature(r, m));
        json_key(w, "kind");
        json_int(w, m->funclike ? SK_FUNCTION : SK_CONSTANT);
        json_key(w, "range");
        json_range(w, &r->snap->tu.sm, r->enc, m->hash_loc ? m->hash_loc
                                                           : m->name_loc,
                   m->end_loc);
        json_key(w, "selectionRange");
        json_range(w, &r->snap->tu.sm, r->enc, m->name_loc,
                   m->name_loc + m->name->len);
        json_end_object(w);
    }
    c_document_symbols(r, w);
    json_end_array(w);
}

/* ---- workspace/symbol (B4_DESIGN.md 7) --------------------------------------- */

enum { WS_LIMIT = 100, WS_NAME_MAX = 255, WS_NO = 1 << 20 };

typedef struct WsCand {
    const char *name, *container, *path;
    Snapshot *snap;          /* whose line tables place it */
    SrcLoc b;
    uint32_t off, len;       /* of the name in its file */
    size_t seq;              /* the unit, in the order searched */
    int rank, kind;          /* ws_match; SymbolKind */
    bool macro;
    /* a project candidate (snap NULL): the hash of the text the record was
     * built from, and its PiSym flags and kind (decision 9) */
    uint64_t fhash;
    uint8_t pflags, pkind;
} WsCand;
typedef VEC(WsCand) WsVec;

/* clangd's FuzzyMatch (see B4_DESIGN.md 7): a character's type is lower (a
 * letter or a digit), upper, or other; its role is a segment's head (the
 * first character, one after other, a lower-to-upper change, the last capital
 * of a run of capitals before a lower one), a separator (other), or a tail. */
enum { WR_TAIL, WR_HEAD, WR_SEP };

static int ws_type(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? 2
         : (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80 ? 1 : 3;
}

static void ws_roles(const char *s, size_t n, unsigned char *role)
{
    size_t i;
    for (i = 0; i < n; i++) {
        int t = ws_type((unsigned char)s[i]),
            p = i ? ws_type((unsigned char)s[i - 1]) : 0,
            nx = i + 1 < n ? ws_type((unsigned char)s[i + 1]) : 0;
        role[i] = t == 3 ? WR_SEP
                : !p || p == 3 || (t == 2 && (p == 1 || (p == 2 && nx == 1)))
                  ? WR_HEAD : WR_TAIL;
    }
}

static bool ws_ieq(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
    return true;
}

/* Dex, which serves the symbols of included files (not of a file open as a
 * document): a query is cut into trigrams (its characters in windows of
 * three) and each must be one that the name's walk generates: from a start,
 * the next character, the next head, and from a non-separator the next
 * separator.  Starts: the name's first character and first two heads for
 * unigrams and bigrams; every character but a separator (and the first) for
 * trigrams. */
static size_t ws_next(size_t nn, const unsigned char *role, size_t i,
                      size_t out[3])
{
    size_t j, n = 0;
    if (i + 1 < nn)
        out[n++] = i + 1;
    for (j = i + 1; j < nn; j++)
        if (role[j] == WR_HEAD) {
            if (j != i + 1)
                out[n++] = j;
            break;
        }
    if (role[i] != WR_SEP)
        for (j = i + 1; j < nn; j++)
            if (role[j] == WR_SEP) {
                if (j != i + 1)
                    out[n++] = j;
                break;
            }
    return n;
}

static bool ws_dex(const char *q, size_t nq, const char *name, size_t nn,
                   const unsigned char *role)
{
    size_t i, w, heads = 0;
    if (nq < 3) {
        for (i = 0; i < nn; i++) {
            size_t nx[3], n, k;
            if (role[i] == WR_HEAD)
                heads++;
            if (i && !(role[i] == WR_HEAD && heads <= 2))
                continue;
            if (tolower((unsigned char)name[i]) != tolower((unsigned char)q[0]))
                continue;
            if (nq == 1)
                return true;
            n = ws_next(nn, role, i, nx);
            for (k = 0; k < n; k++)
                if (tolower((unsigned char)name[nx[k]]) ==
                        tolower((unsigned char)q[1]))
                    return true;
        }
        return false;
    }
    for (w = 0; w + 3 <= nq; w++) {
        bool found = false;
        for (i = 0; i < nn && !found; i++) {
            size_t n1[3], n2[3], a, b, c, m1, m2;
            if ((role[i] == WR_SEP && i) ||
                tolower((unsigned char)name[i]) != tolower((unsigned char)q[w]))
                continue;
            m1 = ws_next(nn, role, i, n1);
            for (a = 0; a < m1 && !found; a++) {
                if (tolower((unsigned char)name[n1[a]]) !=
                        tolower((unsigned char)q[w + 1]))
                    continue;
                m2 = ws_next(nn, role, n1[a], n2);
                for (b = 0, c = 0; b < m2; b++)
                    c |= tolower((unsigned char)name[n2[b]]) ==
                         tolower((unsigned char)q[w + 2]);
                found = c;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

/* How well q matches name: -1 not at all, else lower is better: 0 exact, 1
 * exact ignoring case, 2 a prefix (or an empty query), else 3 + the jumps of
 * the best match.  A match takes the query's characters in order, ignoring
 * case; one that does not follow the previous directly (a jump; the first
 * too) must be at a head, or be a capital of a name that has a lower-case
 * character or digit (clangd's allowMatch).  plain: the name is of a file
 * open as a document, which clangd matches with that rule alone; else (an
 * included file's symbol) the query's trigrams must also be ones clangd's
 * index generates (ws_dex): `lf` matches `luaC_fix` in the open file but not
 * in a header.  Digits are lower-case: no segment starts at a letter-digit
 * change (`a` does not match IPv4addr).  Names over WS_NAME_MAX bytes only
 * match as a prefix. */
static int ws_match(const char *q, const char *name, bool plain)
{
    size_t nq = strlen(q), nn = strlen(name), i, j;
    unsigned char role[WS_NAME_MAX];
    int prev[WS_NAME_MAX], cur[WS_NAME_MAX], best, m;
    bool lowers = false;
    if (!nq)
        return 2;
    if (nq > nn)
        return -1;
    if (nq == nn && ws_ieq(q, name, nn))
        return memcmp(q, name, nn) ? 1 : 0;
    if (ws_ieq(q, name, nq))
        return 2;
    if (nn > WS_NAME_MAX)
        return -1;
    ws_roles(name, nn, role);
    if (!plain && !ws_dex(q, nq, name, nn, role))
        return -1;
    for (j = 0; j < nn; j++)
        lowers |= ws_type((unsigned char)name[j]) == 1;
    for (i = 0; i < nq; i++) {          /* jumps so far, q[0..i] ending at j */
        best = WS_NO;           /* the least of prev[k], k <= j - 2 */
        for (j = 0; j < nn; j++) {
            int c = WS_NO;
            if (tolower((unsigned char)name[j]) == tolower((unsigned char)q[i])) {
                if (i && j && prev[j - 1] < WS_NO)
                    c = prev[j - 1];                 /* right after the previous */
                if ((role[j] != WR_TAIL || (ws_type((unsigned char)name[j]) == 2 &&
                                            lowers)) && (!i || best < WS_NO) &&
                        (!i ? 0 : best + 1) < c)
                    c = i ? best + 1 : 0;
            }
            cur[j] = c;
            if (i && j && prev[j - 1] < best)
                best = prev[j - 1];
        }
        memcpy(prev, cur, nn * sizeof *prev);
    }
    for (m = WS_NO, j = 0; j < nn; j++)
        if (prev[j] < m)
            m = prev[j];
    return m >= WS_NO ? -1 : 3 + m;
}

/* Ranking: match quality, C declarations before macros, a shorter name, the
 * name, then the place, so that the copies of one symbol are adjacent. */
static int ws_cmp(const void *a, const void *b)
{
    const WsCand *x = a, *y = b;
    int c;
    if (x->rank != y->rank)
        return x->rank < y->rank ? -1 : 1;
    if (x->macro != y->macro)
        return x->macro ? 1 : -1;
    if (strlen(x->name) != strlen(y->name))
        return strlen(x->name) < strlen(y->name) ? -1 : 1;
    if ((c = strcmp(x->name, y->name)) || (c = strcmp(x->path, y->path)))
        return c;
    if (x->off != y->off)
        return x->off < y->off ? -1 : 1;
    return x->seq == y->seq ? 0 : x->seq < y->seq ? -1 : 1;
}

/* Whether path is a document the editor has open (WsOpen). */
typedef struct WsOpen {
    const char *const *paths;
    size_t n;
} WsOpen;

static bool ws_is_open(const WsOpen *o, const char *path)
{
    size_t i;
    for (i = 0; i < o->n; i++)
        if (!strcmp(o->paths[i], path))
            return true;
    return false;
}

/* Appends to out the symbols of one unit that match q: its user macros, and
 * its C declarations if it has an index (the document symbols' selection,
 * cindex_symbol_event; one per decl, at its definition if any). */
static void ws_unit(const WsUnit *u, size_t seq, const char *q,
                    const WsOpen *open, WsVec *out)
{
    Snapshot *s = u->snap;
    SrcMgr *sm = &s->tu.sm;
    const CIndex *ix = u->cx;
    SrcFile **fmap = NULL;
    size_t i, nf;
    uint32_t d, j;
    for (i = 0; i < s->tu.pp.macros.len; i++) {
        Macro *m = s->tu.pp.macros.data[i];
        SrcFile *f = m->name_loc && !m->alias_of ? srcmgr_file_of(sm, m->name_loc)
                                                 : NULL;
        int rank;
        if (!f || f->kind != SF_USER ||
            (rank = ws_match(q, m->name->str, ws_is_open(open, f->path))) < 0)
            continue;
        vec_push(out, ((WsCand){m->name->str, "", f->path, s, m->name_loc,
                                (uint32_t)(m->name_loc - f->base), m->name->len,
                                seq, rank, m->funclike ? SK_FUNCTION : SK_CONSTANT,
                                true, 0, 0, 0}));
    }
    if (!ix)
        return;
    fmap = xcalloc(ix->nfiles + 1, sizeof *fmap);   /* the files placed here */
    for (i = 0, nf = srcmgr_nfiles(sm); i < nf; i++) {
        SrcFile *f = srcmgr_file(sm, (uint32_t)i);
        int fi = f->kind == SF_USER ? cindex_file(ix, f->path) : -1;
        if (fi >= 0 && !ix->files[fi].stale)
            fmap[fi] = f;
    }
    for (d = 0; d < ix->ndecls; d++) {
        const CIdxDecl *x = &ix->decls[d];
        const char *name = cindex_name(ix, d);
        uint32_t pick = CIX_NONE;
        int rank;
        if (!x->name || x->kind == CIK_PARAM || x->kind == CIK_LABEL ||
            ws_match(q, name, true) < 0)     /* the looser rule: a first cut */
            continue;
        for (j = ix->by_decl_start[d]; j < ix->by_decl_start[d + 1]; j++) {
            uint32_t k = ix->by_decl[j], sc;
            bool def = (ix->ev[k].flags & CIX_ROLE) == CIX_DEF;
            if (!fmap[ix->ev[k].file] || !cindex_symbol_event(ix, k, &sc))
                continue;
            if (pick == CIX_NONE || def)
                pick = k;
            if (def)
                break;
        }
        if (pick != CIX_NONE) {
            const CIdxEvent *e = &ix->ev[pick];
            SrcFile *f = fmap[e->file];
            if ((rank = ws_match(q, name, ws_is_open(open, f->path))) < 0)
                continue;
            vec_push(out, ((WsCand){name, x->parent ? cindex_name(ix, x->parent - 1)
                                                    : "",
                                    f->path, s, f->base + e->off, e->off, e->len,
                                    seq, rank, C_KINDS[x->kind].sk, false, 0, 0, 0}));
        }
    }
    free(fmap);
}

/* Appends the project's names that match q (10.1): the same matcher, over
 * the name list of every record; a record of an open document's file is
 * matched as the open units' symbols are (plain). */
static void ws_proj(ProjIndex *pi, size_t seq, const char *q, const WsOpen *open,
                    WsVec *out)
{
    size_t i, n = pi_nrecs(pi);
    for (i = 0; i < n; i++) {
        const PiRec *r = pi_rec(pi, i);
        bool plain = ws_is_open(open, r->path);
        uint32_t j;
        for (j = 0; j < r->nsyms; j++) {
            const PiSym *s = &r->syms[j];
            int rank = ws_match(q, s->name, plain);
            bool macro = (s->flags & PIF_MACRO) != 0;
            if (rank < 0)
                continue;
            vec_push(out, ((WsCand){s->name, s->container, r->path, NULL, 0, s->off,
                                    s->len, seq, rank,
                                    macro ? (s->flags & PIF_FUNC ? SK_FUNCTION : SK_CONSTANT)
                                          : C_KINDS[s->kind].sk,
                                    macro, r->fhash, s->flags, s->kind}));
        }
    }
}

static uint64_t ws_key(const WsCand *c)
{
    uint64_t h = 1469598103934665603ull ^ c->pkind;
    const char *p;
    for (p = c->name; *p; p++)
        h = (h ^ (unsigned char)*p) * 1099511628211ull;
    return h;
}

/* Decision 9: a declaration of a function or object with external linkage is
 * dropped when the candidates hold its definition (same name and kind): the
 * definition is the symbol. Only project candidates, v[from..), are looked at. */
static void ws_drop_decls(WsVec *v, size_t from)
{
    typedef struct { const char *name; uint8_t kind; } Def;    /* what the compaction below cannot move */
    size_t i, nb = 16, keep = from;
    Def *tab;
    while (nb < 2 * (v->len - from + 1))
        nb *= 2;
    tab = xcalloc(nb, sizeof *tab);
    for (i = from; i < v->len; i++) {
        const WsCand *c = &v->data[i];
        size_t h;
        if (!(c->pflags & PIF_EXT) || !(c->pflags & PIF_DEF))
            continue;
        for (h = ws_key(c) & (nb - 1); tab[h].name; h = (h + 1) & (nb - 1))
            if (tab[h].kind == c->pkind && !strcmp(tab[h].name, c->name))
                break;
        tab[h].name = c->name;
        tab[h].kind = c->pkind;
    }
    for (i = from; i < v->len; i++) {
        const WsCand *c = &v->data[i];
        size_t h;
        bool dup = false;
        if ((c->pflags & PIF_EXT) && !(c->pflags & PIF_DEF))
            for (h = ws_key(c) & (nb - 1); tab[h].name && !dup; h = (h + 1) & (nb - 1))
                dup = tab[h].kind == c->pkind && !strcmp(tab[h].name, c->name);
        if (!dup)
            v->data[keep++] = *c;
    }
    free(tab);
    v->len = keep;
}

/* The text of a file a project candidate is in, for placing it (10.3). */
typedef struct WsFile {
    const char *path;
    char *cur;                   /* the client's text if open, else the disk's */
    size_t curlen;
    uint64_t curhash;
    bool open, disk_loaded, edit_done;
    char *disk;                  /* open: the disk's text, when it is needed */
    size_t disklen;
    uint64_t diskhash;
    CIdxEdit edit;               /* disk to cur */
    LineTable lines;             /* of cur, built for the first position asked */
    bool lines_built;
} WsFile;
typedef struct WsFiles {
    VEC(WsFile *) v;
    WsDocText doc_text;
    bool hint;                   /* a file changed behind the index's back */
} WsFiles;

static WsFile *ws_file(WsFiles *fs, const char *path)
{
    WsFile *f;
    size_t i;
    for (i = 0; i < fs->v.len; i++)
        if (!strcmp(fs->v.data[i]->path, path))
            return fs->v.data[i];
    f = xcalloc(1, sizeof *f);
    f->path = path;
    if (fs->doc_text && (f->cur = fs->doc_text(path, &f->curlen)) != NULL)
        f->open = true;
    else
        f->cur = read_file_nobom(path, &f->curlen, NULL);
    if (f->cur)
        f->curhash = cindex_hash(f->cur, f->curlen);
    vec_push(&fs->v, f);
    return f;
}

/* Where candidate c is in the text the client has (10.3, decision 8): as it
 * is if that text is what the record was built from; for an open document
 * whose disk text is, mapped through the edit between them, unless the edit
 * touches it; else nowhere. */
static bool ws_place(WsFiles *fs, const WsCand *c, uint32_t *off)
{
    WsFile *f = ws_file(fs, c->path);
    if (!f->cur)
        return false;
    if (f->curhash == c->fhash) {
        *off = c->off;
        return true;
    }
    if (!f->open) {
        fs->hint = true;
        return false;
    }
    if (!f->disk_loaded) {
        f->disk_loaded = true;
        f->disk = read_file_nobom(f->path, &f->disklen, NULL);
        if (f->disk)
            f->diskhash = cindex_hash(f->disk, f->disklen);
    }
    if (!f->disk || f->diskhash != c->fhash) {
        fs->hint = true;
        return false;
    }
    if (!f->edit_done) {
        f->edit_done = true;
        cindex_text_edit(f->disk, f->disklen, f->cur, f->curlen, &f->edit);
    }
    if (!f->edit.same && c->off < f->edit.old_end && c->off + c->len > f->edit.pre)
        return false;
    *off = cindex_edit_map(&f->edit, c->off);
    return true;
}

static void ws_files_free(WsFiles *fs)
{
    size_t i;
    for (i = 0; i < fs->v.len; i++) {
        free(fs->v.data[i]->cur);
        free(fs->v.data[i]->disk);
        vec_free(&fs->v.data[i]->lines);
        free(fs->v.data[i]);
    }
    vec_free(&fs->v);
}

static void json_pos_at(JsonWriter *w, WsFile *f, PosEncoding enc, size_t off)
{
    uint32_t line, ch;
    if (!f->lines_built) {
        line_table_build(&f->lines, f->cur, f->curlen);
        f->lines_built = true;
    }
    line_table_pos(&f->lines, f->cur, f->curlen, off, enc, &line, &ch);
    json_begin_object(w);
    json_key(w, "line");
    json_int(w, line);
    json_key(w, "character");
    json_int(w, ch);
    json_end_object(w);
}

/* The results sent so far, by (path, offset as placed, name): the one symbol seen
 * through several units, configurations, or an open file and its edit is sent
 * once.  Inserts c unless it is there; true if it was. */
typedef struct WsSent {
    const WsCand *c;
    uint32_t off;
} WsSent;
enum { WS_SENT_SLOTS = 256 };            /* a power of 2 over WS_LIMIT */

static bool ws_seen(WsSent *tab, const WsCand *c, uint32_t off)
{
    uint64_t h = 1469598103934665603ull ^ off;
    const char *p;
    for (p = c->name; *p; p++)
        h = (h ^ (unsigned char)*p) * 1099511628211ull;
    for (p = c->path; *p; p++)
        h = (h ^ (unsigned char)*p) * 1099511628211ull;
    for (h &= WS_SENT_SLOTS - 1; tab[h].c; h = (h + 1) & (WS_SENT_SLOTS - 1))
        if (tab[h].off == off && !strcmp(tab[h].c->name, c->name) &&
            !strcmp(tab[h].c->path, c->path))
            return true;
    tab[h].c = c;
    tab[h].off = off;
    return false;
}

void lsp_workspace_symbols(const WsUnit *us, size_t n, const char *const *docs,
                           size_t ndocs, ProjIndex *pi, WsDocText doc_text,
                           const char *query, PosEncoding enc, Arena *a, JsonWriter *w)
{
    WsVec v = {0};
    WsOpen open = {docs, ndocs};
    WsFiles files = {{0}, doc_text, false};
    WsSent sent[WS_SENT_SLOTS] = {{0}};
    Req r;
    size_t i, shown = 0;
    for (i = 0; i < n; i++)
        ws_unit(&us[i], i, query, &open, &v);
    if (pi) {                    /* held to the end: the names are in its mappings */
        size_t from = v.len;
        pi_rdlock(pi);
        ws_proj(pi, n, query, &open, &v);
        ws_drop_decls(&v, from);
    }
    if (v.len)
        qsort(v.data, v.len, sizeof *v.data, ws_cmp);
    memset(&r, 0, sizeof r);
    r.arena = a;
    r.enc = enc;
    json_begin_array(w);
    for (i = 0; i < v.len && shown < WS_LIMIT; i++) {
        const WsCand *c = &v.data[i];
        uint32_t off = c->off;
        if (!c->snap && !ws_place(&files, c, &off))
            continue;                  /* the project's text is not the file's */
        if (ws_seen(sent, c, off))
            continue;
        r.snap = c->snap;
        json_begin_object(w);
        json_key(w, "name");
        json_str(w, c->name);
        json_key(w, "kind");
        json_int(w, c->kind);
        json_key(w, "location");
        if (c->snap) {
            json_location(w, &r, c->b, c->b + c->len);
        } else {
            WsFile *f = ws_file(&files, c->path);
            json_begin_object(w);
            json_key(w, "uri");
            json_str(w, path_to_uri(a, c->path));
            json_key(w, "range");
            json_begin_object(w);
            json_key(w, "start");
            json_pos_at(w, f, enc, off);
            json_key(w, "end");
            json_pos_at(w, f, enc, (size_t)off + c->len);
            json_end_object(w);
            json_end_object(w);
        }
        json_key(w, "containerName");
        json_str(w, c->container);
        json_end_object(w);
        shown++;
    }
    json_end_array(w);
    if (pi)
        pi_unlock(pi);
    if (files.hint)
        project_hint_sweep();
    ws_files_free(&files);
    vec_free(&v);
}

/* ---- semantic tokens -------------------------------------------------------- */

typedef struct STok {
    SrcLoc loc;
    uint32_t len;
    int type, mods;
    bool from_c;             /* from the C index, else the macro index */
} STok;

static int stok_cmp(const void *a, const void *b)
{
    const STok *x = a, *y = b;
    if (x->loc != y->loc)
        return x->loc < y->loc ? -1 : 1;
    /* fresh, the two indexes never start a token at one place; a carried C
     * token can meet a macro token (a new #define of its name): the macro's
     * wins */
    if (x->from_c != y->from_c)
        return x->from_c - y->from_c;
    return y->mods - x->mods; /* the declaration first at a tie */
}


/* The encoded tokens (LSP's 5 integers each, relative to the previous
 * token) of the refs, parameters and definitions in [b, e]. */
static void semantic_data(Req *r, SrcLoc b, SrcLoc e, U32Vec *out)
{
    Index *ix = &r->snap->ix;
    PP *pp = &r->snap->tu.pp;
    VEC(STok) v = {0};
    size_t i, nrefs;
    SrcLoc prev_end = 0;
    uint32_t pl = 0, pc = 0;
    IdxRef *refs;
    int fi;
#define IN_R(l) ((l) >= b && (l) <= e)
    nrefs = index_range_refs(ix, b, e, &refs);
    for (i = 0; i < nrefs; i++) {
        IdxRef *ref = &refs[i];
        STok s = {0};
        if (!ref->len)
            continue;
        s.loc = ref->loc;
        s.len = ref->len;
        s.type = ST_MACRO;
        s.mods = 0;
        vec_push(&v, s);
    }
    for (i = 0; i < ix->params.len; i++) {
        IdxParamRef *p = &ix->params.data[i];
        STok s = {0};
        if (!IN_R(p->loc))
            continue;
        s.loc = p->loc;
        s.len = p->len;
        s.type = ST_PARAM;
        s.mods = 0;
        vec_push(&v, s);
    }
    for (i = 0; i < pp->macros.len; i++) {
        Macro *m = pp->macros.data[i];
        STok s = {0};
        int k;
        if (m->alias_of || !m->name_loc || !IN_R(m->name_loc))
            continue;
        s.loc = m->name_loc;
        s.len = m->name->len;
        s.type = ST_MACRO;
        s.mods = SM_DECLARATION;
        vec_push(&v, s);
        for (k = 0; k < m->nparams; k++) {
            if (!m->param_locs[k] || !IN_R(m->param_locs[k]))
                continue;
            s.loc = m->param_locs[k];
            s.len = m->params[k]->len;
            s.type = ST_PARAM;
            vec_push(&v, s);
        }
    }
#undef IN_R
    if ((fi = c_file(r)) >= 0 && e >= r->file->base) {
        /* the C names, but those spelled in a #define body (one token there
         * names an entity per expansion) or formed by ## (B3_DESIGN.md 5.4) */
        const CIndex *cx = r->cidx;
        uint32_t k = cindex_lower(cx, (uint32_t)fi,
                                  b > r->file->base ? b - r->file->base : 0);
        for (; k < cx->nev && cx->ev[k].file == (uint32_t)fi &&
               r->file->base + cx->ev[k].off <= e; k++) {
            const CIdxEvent *ce = &cx->ev[k];
            const CIdxDecl *x = &cx->decls[ce->decl];
            STok s = {0};
            if ((ce->flags & (CIX_MACRO_BODY | CIX_AT_EXPANSION | CIX_SYSTEM)) ||
                C_KINDS[x->kind].st < 0 || !ce->len)
                continue;
            s.from_c = true;
            s.loc = r->file->base + ce->off;
            s.len = ce->len;
            s.type = C_KINDS[x->kind].st;
            s.mods = ((ce->flags & CIX_ROLE) != CIX_REF ? SM_DECLARATION : 0) |
                     (x->flags & CIDF_READONLY ? SM_READONLY : 0) |
                     (x->flags & CIDF_STATIC ? SM_STATIC : 0);
            vec_push(&v, s);
        }
    }
    if (v.len)
        qsort(v.data, v.len, sizeof *v.data, stok_cmp);
    for (i = 0; i < v.len; i++) {
        STok *s = &v.data[i];
        uint32_t l, c, el, ec;
        if (s->loc < prev_end)
            continue; /* overlapping or duplicate */
        loc_to_pos(&r->snap->tu.sm, s->loc, r->enc, &l, &c);
        loc_to_pos(&r->snap->tu.sm, s->loc + s->len, r->enc, &el, &ec);
        if (el != l)
            continue; /* tokens are single-line */
        vec_push(out, l - pl);
        vec_push(out, l == pl ? c - pc : c);
        vec_push(out, ec - c);
        vec_push(out, (uint32_t)s->type);
        vec_push(out, (uint32_t)s->mods);
        pl = l;
        pc = c;
        prev_end = s->loc + s->len;
    }
    vec_free(&v);
}

static void json_u32s(JsonWriter *w, const uint32_t *d, size_t n)
{
    size_t i;
    json_begin_array(w);
    for (i = 0; i < n; i++)
        json_int(w, d[i]);
    json_end_array(w);
}

/* Whole-document results, per document: what the client last got (for
 * deltas) and the snapshot they were computed on (repeated requests are
 * free).  Requests are handled on one thread. */
typedef struct TokCache {
    char *path;
    const void *snap;          /* identity only; may have been freed */
    long long gen;
    PosEncoding enc;
    uint32_t cserial;          /* serial of the C index used (0: none): a
                                  carried one is not reused after the
                                  snapshot's own publishes */
    unsigned long id;          /* resultId */
    U32Vec data;
} TokCache;

static VEC(TokCache) tok_cache;
static unsigned long tok_next_id;

static TokCache *tok_entry(const char *path)
{
    size_t i;
    TokCache t;
    for (i = 0; i < tok_cache.len; i++)
        if (!strcmp(tok_cache.data[i].path, path))
            return &tok_cache.data[i];
    memset(&t, 0, sizeof t);
    t.path = xstrdup(path);
    vec_push(&tok_cache, t);
    return &vec_last(&tok_cache);
}

void lsp_forget_tokens(const char *path)
{
    size_t i;
    for (i = 0; i < tok_cache.len; i++)
        if (!path || !strcmp(tok_cache.data[i].path, path)) {
            free(tok_cache.data[i].path);
            vec_free(&tok_cache.data[i].data);
            tok_cache.data[i--] = tok_cache.data[--tok_cache.len];
        }
    if (!path)
        vec_free(&tok_cache);
}

/* full, or full/delta against previous_id (NULL: full) */
void lsp_semantic_tokens(Req *r, JsonWriter *w, const char *previous_id)
{
    TokCache *c = tok_entry(r->path);
    U32Vec nd = {0};
    char id[32];
    bool fresh = c->snap == r->snap && c->gen == r->snap->gen &&
                 c->enc == r->enc && c->cserial == (r->cidx ? r->cidx->serial : 0) && c->id;
    unsigned long prev = previous_id ? strtoul(previous_id, NULL, 10) : 0;
    if (fresh) {
        if (prev == c->id) { /* nothing changed since */
            json_begin_object(w);
            json_key(w, "resultId");
            snprintf(id, sizeof id, "%lu", c->id);
            json_str(w, id);
            json_key(w, "edits");
            json_begin_array(w);
            json_end_array(w);
            json_end_object(w);
            return;
        }
        if (!previous_id) {
            json_begin_object(w);
            json_key(w, "resultId");
            snprintf(id, sizeof id, "%lu", c->id);
            json_str(w, id);
            json_key(w, "data");
            json_u32s(w, c->data.data, c->data.len);
            json_end_object(w);
            return;
        }
    }
    semantic_data(r, r->file->base, r->file->base + r->file->size, &nd);
    json_begin_object(w);
    json_key(w, "resultId");
    snprintf(id, sizeof id, "%lu", ++tok_next_id);
    json_str(w, id);
    if (previous_id && prev && prev == c->id) {
        /* one edit: the part between the common prefix and suffix */
        size_t p = 0, q = 0, on = c->data.len, nn = nd.len;
        while (p < on && p < nn && c->data.data[p] == nd.data[p])
            p++;
        while (q < on - p && q < nn - p &&
               c->data.data[on - 1 - q] == nd.data[nn - 1 - q])
            q++;
        json_key(w, "edits");
        json_begin_array(w);
        if (p != on || p != nn) {
            json_begin_object(w);
            json_key(w, "start");
            json_int(w, (long long)p);
            json_key(w, "deleteCount");
            json_int(w, (long long)(on - p - q));
            json_key(w, "data");
            json_u32s(w, nd.data + p, nn - p - q);
            json_end_object(w);
        }
        json_end_array(w);
    } else {
        json_key(w, "data");
        json_u32s(w, nd.data, nd.len);
    }
    json_end_object(w);
    vec_free(&c->data);
    c->data = nd;
    c->id = tok_next_id;
    c->snap = r->snap;
    c->gen = r->snap->gen;
    c->enc = r->enc;
    c->cserial = r->cidx ? r->cidx->serial : 0;
}

void lsp_semantic_tokens_range(Req *r, JsonWriter *w)
{
    const JsonValue *rg = json_get(r->params, "range");
    SrcLoc b = req_loc(r, json_get(rg, "start")),
           e = req_loc(r, json_get(rg, "end"));
    U32Vec d = {0};
    semantic_data(r, b, e < b ? b : e, &d);
    json_begin_object(w);
    json_key(w, "data");
    json_u32s(w, d.data, d.len);
    json_end_object(w);
    vec_free(&d);
}

/* ---- folding ----------------------------------------------------------------- */

void lsp_folding(Req *r, JsonWriter *w)
{
    Index *ix = &r->snap->ix;
    PP *pp = &r->snap->tu.pp;
    size_t i;
    json_begin_array(w);
    for (i = 0; i < ix->blocks.len; i++) {
        IdxBlock *b = &ix->blocks.data[i];
        uint32_t l1, c1, l2, c2;
        if (!in_file(r->file, b->begin))
            continue;
        loc_to_pos(&r->snap->tu.sm, b->begin, r->enc, &l1, &c1);
        loc_to_pos(&r->snap->tu.sm, b->end, r->enc, &l2, &c2);
        if (l2 <= l1 + 1)
            continue;
        json_begin_object(w);
        json_key(w, "startLine");
        json_int(w, l1);
        json_key(w, "endLine");
        json_int(w, l2 - 1); /* the #endif line stays visible */
        json_key(w, "kind");
        json_str(w, "region");
        json_end_object(w);
    }
    for (i = 0; i < pp->macros.len; i++) { /* multi-line #defines */
        Macro *m = pp->macros.data[i];
        uint32_t l1, c1, l2, c2;
        if (m->alias_of || !in_file(r->file, m->name_loc) || !m->hash_loc)
            continue;
        loc_to_pos(&r->snap->tu.sm, m->hash_loc, r->enc, &l1, &c1);
        loc_to_pos(&r->snap->tu.sm, m->end_loc, r->enc, &l2, &c2);
        if (l2 <= l1)
            continue;
        json_begin_object(w);
        json_key(w, "startLine");
        json_int(w, l1);
        json_key(w, "endLine");
        json_int(w, l2);
        json_key(w, "kind");
        json_str(w, "region");
        json_end_object(w);
    }
    json_end_array(w);
}

/* ---- rename ------------------------------------------------------------------ */

/* Why the target cannot be renamed, or NULL. */
static const char *rename_blocker(Req *r, const IdxTarget *t, IdxRef *refs,
                                  size_t n)
{
    SrcMgr *sm = &r->snap->tu.sm;
    size_t i;
    int k;
    if (t->kind != TGT_MACRO && t->kind != TGT_PARAM)
        return "not a macro or macro parameter";
    for (k = 0; k < t->nmacros; k++) {
        SrcFile *f = srcmgr_file_of(sm, t->macros[k]->name_loc);
        if (t->macros[k]->builtin || t->macros[k]->predefined || !f ||
            f->kind != SF_USER || f->system_header)
            return "the macro is predefined or defined in a system header";
    }
    if (t->kind == TGT_MACRO && !t->nmacros)
        return "not a macro here";
    for (i = 0; i < n; i++) {
        if (refs[i].flags & IREF_PASTED) {
            SrcFile *f = srcmgr_file_of(sm, refs[i].loc);
            uint32_t l = 0, c = 0;
            if (f)
                srcmgr_linecol(f, refs[i].loc, &l, &c);
            return arena_printf(r->arena, "the name is formed by ## at %s:%u; "
                                "renaming would not rename that use",
                                f ? f->path : "?", l);
        }
        if (!real_loc(sm, refs[i].loc))
            continue;
        if (srcmgr_file_of(sm, refs[i].loc)->system_header)
            return "a reference is in a system header";
    }
    return NULL;
}

/* The C name at the cursor (B2 phase 4) as the range a rename edits there,
 * from the check's index (cindex_rename_plan), or why it cannot be renamed.
 * The index must be the newest edit's: its offsets are the edits'. */
static const char *c_rename_at(Req *r, SrcLoc at, SrcLoc *b, SrcLoc *e)
{
    const SrcFile *mf = index_find_file(&r->snap->ix, r->snap->main);
    uint32_t *ev = NULL, first;
    size_t n;
    char msg[512];
    const char *err;
    const CIdxEvent *x;
    if (!r->c_fresh || !mf)
        return "the C symbol index is not ready; retry";
    err = cindex_rename_plan(r->cidx, &r->snap->tu.sm, r->file->path,
                             at - r->file->base, mf->path, &ev, &n, msg,
                             sizeof msg);
    free(ev);
    if (err)
        return arena_strdup(r->arena, err);
    cindex_at(r->cidx, (uint32_t)cindex_file(r->cidx, r->file->path),
              at - r->file->base, &first);
    x = &r->cidx->ev[first];
    *b = r->file->base + x->off;
    *e = *b + x->len;
    return NULL;
}

/* A macro or macro parameter is renamed when the macro index answers for
 * the name (not weakly), else a C name; null where there is no entity. */
bool lsp_prepare_rename(Req *r, JsonWriter *w, const char **err)
{
    SrcLoc at = cursor(r);
    IdxTarget t = index_resolve(&r->snap->ix, at);
    IdxRef *refs = NULL;
    size_t n = 0;
    SrcLoc b = t.range.begin, e = t.range.end;
    uint32_t d;
    if (t.kind == TGT_NONE || t.weak) {
        if (r->c_fresh && !cindex_decls_at(r->cidx, r->file->path,
                                           at - r->file->base, &d, 1)) {
            json_null(w); /* no entity here */
            return true;
        }
        if ((*err = c_rename_at(r, at, &b, &e)) != NULL)
            return false;
    } else {
        if (t.kind == TGT_MACRO || t.kind == TGT_PARAM)
            n = index_references(&r->snap->ix, &t, &refs);
        if ((*err = rename_blocker(r, &t, refs, n)) != NULL)
            return false;
        if (!e) {
            *err = "no name here";
            return false;
        }
    }
    json_begin_object(w);
    json_key(w, "range");
    json_range(w, &r->snap->tu.sm, r->enc, b, e);
    json_key(w, "placeholder");
    json_str(w, arena_strndup(r->arena, r->file->buf + (b - r->file->base),
                              e - b));
    json_end_object(w);
    return true;
}

static int ref_file_cmp(const void *a, const void *b)
{
    const IdxRef *x = a, *y = b;
    return x->loc < y->loc ? -1 : x->loc > y->loc;
}

/* A C name: c_rename's edits, all in the unit's main file. */
static bool c_rename_edits(Req *r, JsonWriter *w, SrcLoc at, const char *name,
                           const char **err)
{
    SrcFile *mf = index_find_file(&r->snap->ix, r->snap->main);
    CRename q;
    char *why;
    size_t i;
    if (!r->c_fresh || !mf) {
        *err = "the C symbol index is not ready; retry";
        return false;
    }
    if (!r->on_builder) { /* the checks are the builder's: defer (no output) */
        r->need_builder = true;
        return true;
    }
    memset(&q, 0, sizeof q);
    q.path = r->file->path;
    q.off = at - r->file->base;
    q.name = name;
    if ((why = lsp_check_rename(r->snap, &q)) != NULL) {
        *err = arena_strdup(r->arena, why);
        free(why);
        return false;
    }
    json_begin_object(w);
    json_key(w, "changes");
    json_begin_object(w);
    json_key(w, path_to_uri(r->arena, mf->path));
    json_begin_array(w);
    for (i = 0; i < q.n; i++) {
        json_begin_object(w);
        json_key(w, "range");
        json_range(w, &r->snap->tu.sm, r->enc, mf->base + q.offs[i],
                   mf->base + q.offs[i] + q.len);
        json_key(w, "newText");
        json_str(w, name);
        json_end_object(w);
    }
    json_end_array(w);
    json_end_object(w);
    json_end_object(w);
    free(q.offs);
    return true;
}

bool lsp_rename(Req *r, JsonWriter *w, const char **err)
{
    SrcLoc at = cursor(r);
    IdxTarget t = index_resolve(&r->snap->ix, at);
    const char *name = json_str_of(json_get(r->params, "newName"), "");
    SrcMgr *sm = &r->snap->tu.sm;
    IdxRef *refs = NULL, *sorted;
    size_t n = 0, i;
    SrcFile *cur = NULL;
    SrcLoc last = 0;
    if (t.kind == TGT_NONE || t.weak)
        return c_rename_edits(r, w, at, name, err);
    if (t.kind == TGT_MACRO || t.kind == TGT_PARAM)
        n = index_references(&r->snap->ix, &t, &refs);
    if ((*err = rename_blocker(r, &t, refs, n)) != NULL)
        return false;
    if (!cindex_is_identifier(name)) {
        *err = arena_printf(r->arena, "'%s' is not an identifier", name);
        return false;
    }
    sorted = NEW_ARRAY(r->arena, IdxRef, n + 1);
    if (n)
        memcpy(sorted, refs, sizeof(IdxRef) * n);
    qsort(sorted, n, sizeof *sorted, ref_file_cmp);
    json_begin_object(w);
    json_key(w, "changes");
    json_begin_object(w);
    for (i = 0; i < n; i++) {
        IdxRef *ref = &sorted[i];
        SrcFile *f;
        if (!real_loc(sm, ref->loc) || ref->loc == last || !ref->len)
            continue;
        last = ref->loc;
        f = srcmgr_file_of(sm, ref->loc);
        if (f != cur) {
            if (cur)
                json_end_array(w);
            json_key(w, path_to_uri(r->arena, f->path));
            json_begin_array(w);
            cur = f;
        }
        json_begin_object(w);
        json_key(w, "range");
        json_range(w, sm, r->enc, ref->loc, ref->loc + ref->len);
        json_key(w, "newText");
        json_str(w, name);
        json_end_object(w);
    }
    if (cur)
        json_end_array(w);
    json_end_object(w);
    json_end_object(w);
    return true;
}

/* ---- call hierarchy --------------------------------------------------------- *
 * Macro items from the macro graph; C items (functions, and file-level
 * variables whose initializers call) from the C index's CIX_CALL events
 * (docs/B4_DESIGN.md 5).  An item's data names the document that answered, so
 * a follow-up runs in the same unit even when the item lies in a header. */

/* The document the request is about, as the client wrote its uri. */
static const char *doc_uri(Req *r)
{
    const char *u = json_str_of(json_path(r->params, "textDocument.uri"), NULL);
    if (!u)
        u = json_str_of(json_path(r->params, "item.data.doc"), NULL);
    return u ? u : path_to_uri(r->arena, r->path);
}

static void call_item(Req *r, JsonWriter *w, Macro *m)
{
    SrcMgr *sm = &r->snap->tu.sm;
    json_begin_object(w);
    json_key(w, "name");
    json_str(w, m->name->str);
    json_key(w, "kind");
    json_int(w, m->funclike ? SK_FUNCTION : SK_CONSTANT);
    json_key(w, "detail");
    json_str(w, signature(r, m));
    json_key(w, "uri");
    json_str(w, uri_of(r->arena, sm, m->name_loc));
    json_key(w, "range");
    json_range(w, sm, r->enc, m->hash_loc ? m->hash_loc : m->name_loc,
               m->end_loc ? m->end_loc : m->name_loc + m->name->len);
    json_key(w, "selectionRange");
    json_range(w, sm, r->enc, m->name_loc, m->name_loc + m->name->len);
    json_key(w, "data");
    json_begin_object(w);
    json_key(w, "macro");
    json_int(w, m->id);
    json_key(w, "name");
    json_str(w, m->name->str);
    json_key(w, "doc");
    json_str(w, doc_uri(r));
    json_end_object(w);
    json_end_object(w);
}

/* The snapshot's file of the index's file fi, NULL if it has no line table
 * here or its text differs. */
static SrcFile *c_item_file(Req *r, uint32_t fi)
{
    const CIdxFile *cf = &r->cidx->files[fi];
    return cf->stale ? NULL : index_find_file(&r->snap->ix, cf->path);
}

/* A C entity that can be a call hierarchy item: a function, or a file-level
 * variable (its initializer's calls are its outgoing calls). */
static bool c_item_decl(const CIndex *ix, uint32_t d)
{
    return ix->decls[d].kind == CIK_FUNC ||
           (ix->decls[d].kind == CIK_OBJ && !ix->decls[d].scope);
}

/* The item of decl d at its event ev; false (nothing written) if the event's
 * file cannot be shown. */
static bool c_call_item(Req *r, JsonWriter *w, uint32_t d, uint32_t ev)
{
    const CIndex *ix = r->cidx;
    const CIdxEvent *e = ev < ix->nev ? &ix->ev[ev] : NULL;
    SrcFile *f = e ? c_item_file(r, e->file) : NULL;
    SrcMgr *sm = &r->snap->tu.sm;
    uint32_t rb, re, s;
    if (!f)
        return false;
    rb = e->off;
    re = e->off + e->len;
    if ((s = cindex_scope_at(ix, e->file, e->off)) != 0) {  /* widened root */
        const CIdxScope *root = &ix->scopes[cindex_scope_root(ix, s) - 1];
        rb = root->begin < rb ? root->begin : rb;
        re = root->end > re ? root->end : re;
    }
    json_begin_object(w);
    json_key(w, "name");
    json_str(w, cindex_name(ix, d));
    json_key(w, "kind");
    json_int(w, C_KINDS[ix->decls[d].kind].sk);
    json_key(w, "detail");
    json_str(w, c_detail(r, d));
    json_key(w, "uri");
    json_str(w, path_to_uri(r->arena, f->path));
    json_key(w, "range");
    json_range(w, sm, r->enc, f->base + rb, f->base + re);
    json_key(w, "selectionRange");
    json_range(w, sm, r->enc, f->base + e->off, f->base + e->off + e->len);
    json_key(w, "data");
    json_begin_object(w);
    json_key(w, "c");
    json_int(w, d);
    json_key(w, "name");
    json_str(w, cindex_name(ix, d));
    json_key(w, "doc");
    json_str(w, doc_uri(r));
    json_end_object(w);
    json_end_object(w);
    return true;
}

void lsp_prepare_call_hierarchy(Req *r, JsonWriter *w)
{
    SrcLoc at = cursor(r);
    IdxTarget t = index_resolve(&r->snap->ix, at);
    uint32_t ds[8];
    size_t nd = 0, k;
    int m;
    if (t.kind == TGT_MACRO && t.nmacros && !t.weak) {
        json_begin_array(w);
        for (m = 0; m < t.nmacros; m++)
            if (real_loc(&r->snap->tu.sm, t.macros[m]->name_loc))
                call_item(r, w, t.macros[m]);
        json_end_array(w);
        return;
    }
    if (c_file(r) >= 0)
        nd = cindex_decls_at(r->cidx, r->file->path, at - r->file->base, ds, 8);
    for (k = 0; k < nd; k++)        /* a macro with only history: its answer */
        if (c_item_decl(r->cidx, ds[k]))
            break;
    if (k == nd && t.kind == TGT_MACRO && t.nmacros) {
        json_begin_array(w);
        for (m = 0; m < t.nmacros; m++)
            if (real_loc(&r->snap->tu.sm, t.macros[m]->name_loc))
                call_item(r, w, t.macros[m]);
        json_end_array(w);
        return;
    }
    if (k == nd) {
        json_null(w);
        return;
    }
    json_begin_array(w);
    for (k = 0; k < nd; k++)
        if (c_item_decl(r->cidx, ds[k]))
            c_call_item(r, w, ds[k], cindex_site(r->cidx, ds[k]));
    json_end_array(w);
}

/* The item's file in this snapshot, NULL if it is not there. */
static SrcFile *item_file(Req *r, const JsonValue *item)
{
    const char *uri = json_str_of(json_get(item, "uri"), NULL);
    const char *path = uri ? uri_to_path(r->arena, uri) : NULL;
    return path ? index_find_file(&r->snap->ix, path) : r->file;
}

/* The item's macro in this snapshot: by id, checked by name; else the
 * definition at the item's selection range. */
static Macro *item_macro(Req *r)
{
    const JsonValue *item = json_get(r->params, "item");
    PP *pp = &r->snap->tu.pp;
    long long id = json_int_of(json_path(item, "data.macro"), -1);
    const char *name = json_str_of(json_path(item, "data.name"), "");
    const JsonValue *sel = json_path(item, "selectionRange.start");
    SrcFile *f = item_file(r, item);
    if (id >= 0 && (size_t)id < pp->macros.len &&
        !strcmp(pp->macros.data[id]->name->str, name))
        return pp->macros.data[id];
    if (sel && f) {
        SrcLoc loc = req_loc_in(r, f, sel);
        size_t i;
        for (i = 0; i < pp->macros.len; i++)
            if (pp->macros.data[i]->name_loc == loc)
                return pp->macros.data[i];
    }
    return NULL;
}

/* Where m's replacement list spells name. */
static void spelled_ranges(Req *r, JsonWriter *w, Macro *m, const Ident *name)
{
    uint32_t b;
    bool any = false;
    json_begin_array(w);
    for (b = 0; b < m->body_len; b++) {
        const Tok *t = &m->body[b];
        if (t->kind == TK_IDENT && !(t->flags & TF_PARAM) &&
            pp_ident(&r->snap->tu.pp, t) == name &&
            real_loc(&r->snap->tu.sm, t->loc)) {
            json_range(w, &r->snap->tu.sm, r->enc, t->loc, t->loc + t->len);
            any = true;
        }
    }
    if (!any) /* formed by ##: point at the definition */
        json_range(w, &r->snap->tu.sm, r->enc, m->name_loc,
                   m->name_loc + m->name->len);
    json_end_array(w);
}

/* The C item of a follow-up request (B4_DESIGN.md 5.5): the decl at the
 * item's position with its name, else data.c if it still has that name, else
 * the file-level function or variable of that name (one with a definition
 * first); false if none. */
static bool c_item_resolve(Req *r, uint32_t *out)
{
    const CIndex *ix = r->cidx;
    const JsonValue *item = json_get(r->params, "item");
    const JsonValue *sel = json_path(item, "selectionRange.start");
    const char *name = json_str_of(json_path(item, "data.name"),
                                   json_str_of(json_get(item, "name"), ""));
    long long dc = json_int_of(json_path(item, "data.c"), -1);
    SrcFile *f = item_file(r, item);
    uint32_t ds[8], d, j, any = UINT32_MAX;
    size_t nd, k;
    if (!ix)
        return false;
    if (f && sel && cindex_file(ix, f->path) >= 0) {
        SrcLoc loc = req_loc_in(r, f, sel);
        nd = loc >= f->base ? cindex_decls_at(ix, f->path, loc - f->base, ds, 8) : 0;
        for (k = 0; k < nd; k++)
            if (c_item_decl(ix, ds[k]) && !strcmp(cindex_name(ix, ds[k]), name)) {
                *out = ds[k];
                return true;
            }
    }
    if (dc >= 0 && (uint64_t)dc < ix->ndecls && c_item_decl(ix, (uint32_t)dc) &&
        !strcmp(cindex_name(ix, (uint32_t)dc), name)) {
        *out = (uint32_t)dc;
        return true;
    }
    for (d = 0; d < ix->ndecls; d++) {
        if (!c_item_decl(ix, d) || strcmp(cindex_name(ix, d), name))
            continue;
        if (any == UINT32_MAX)
            any = d;
        for (j = ix->by_decl_start[d]; j < ix->by_decl_start[d + 1]; j++)
            if ((ix->ev[ix->by_decl[j]].flags & CIX_ROLE) == CIX_DEF) {
                *out = d;
                return true;
            }
    }
    *out = any;
    return any != UINT32_MAX;
}

/* incomingCalls / outgoingCalls of the C decl d (B4_DESIGN.md 5.4): one entry
 * per calling function (or variable) / callee, with the call sites. */
static void c_calls(Req *r, JsonWriter *w, bool incoming, uint32_t d)
{
    const CIndex *ix = r->cidx;
    SrcMgr *sm = &r->snap->tu.sm;
    CIdxCall *calls = NULL;
    size_t n = 0, i, j, k;
    if (incoming && ix->decls[d].kind == CIK_FUNC)
        n = cindex_calls_to(ix, d, &calls);
    else if (!incoming)
        n = cindex_calls_in(ix, cindex_site(ix, d), &calls);
    json_begin_array(w);
    for (i = 0; i < n; i = j) {
        SrcFile *sf = calls[i].other == CIX_NONE ? NULL
                                                 : c_item_file(r, calls[i].file);
        for (j = i + 1; j < n && calls[j].other == calls[i].other &&
                        calls[j].other_ev == calls[i].other_ev; j++)
            ;
        if (!sf || calls[i].other_ev >= ix->nev ||
            !c_item_file(r, ix->ev[calls[i].other_ev].file))
            continue;
        json_begin_object(w);
        json_key(w, incoming ? "from" : "to");
        c_call_item(r, w, calls[i].other, calls[i].other_ev);
        json_key(w, "fromRanges");
        json_begin_array(w);
        for (k = i; k < j; k++) {
            json_range(w, sm, r->enc, sf->base + calls[k].off,
                       sf->base + calls[k].off + calls[k].len);
        }
        json_end_array(w);
        json_end_object(w);
    }
    json_end_array(w);
    free(calls);
}

void lsp_calls(Req *r, JsonWriter *w, bool incoming)
{
    const JsonValue *item = json_get(r->params, "item");
    Macro *m = json_path(item, "data.c") ? NULL : item_macro(r);
    IdxCall *calls;
    size_t n, i;
    uint32_t d;
    if (!m) {
        if (c_item_resolve(r, &d))
            c_calls(r, w, incoming, d);
        else
            json_null(w);
        return;
    }
    n = incoming ? index_callers(&r->snap->ix, &r->snap->graph, m, &calls)
                 : index_callees(&r->snap->ix, &r->snap->graph, m, &calls);
    json_begin_array(w);
    for (i = 0; i < n; i++) {
        Macro *o = calls[i].macro;
        if (!o || !real_loc(&r->snap->tu.sm, o->name_loc))
            continue;
        json_begin_object(w);
        json_key(w, incoming ? "from" : "to");
        call_item(r, w, o);
        json_key(w, "fromRanges");
        if (incoming)
            spelled_ranges(r, w, o, m->name); /* in the caller */
        else
            spelled_ranges(r, w, m, o->name); /* in m */
        json_end_object(w);
    }
    json_end_array(w);
}

/* ---- signature help ---------------------------------------------------------- *
 * While an invocation is being typed it is usually incomplete, so this
 * reads the editor text: back from the cursor to the unmatched '(' and the
 * name before it, counting top-level commas on the way.  A function-like
 * macro defined there answers, else the C entity (B3_DESIGN.md 5.3). */

typedef struct Sig {
    CSig c;
    const char *doc;
} Sig;

static void sig_free(Sig *s)
{
    csig_free(&s->c);
}

static void macro_sig(Req *r, Macro *m, Sig *s)
{
    int k;
    sb_printf(&s->c.label, "%s(", m->name->str);
    for (k = 0; k < m->nparams; k++) {
        const char *pn = m->variadic && k == m->nparams - 1 &&
                                 !m->gnu_named_variadic
                             ? "..." : m->params[k]->str;
        csig_param(&s->c, pn, strlen(pn));
        if (m->gnu_named_variadic && k == m->nparams - 1) {
            sb_puts(&s->c.label, "...");
            s->c.offs.data[s->c.offs.len - 1] = (uint32_t)s->c.label.len;
        }
    }
    sb_putc(&s->c.label, ')');
    s->c.variadic = m->variadic;
    s->doc = arena_printf(r->arena, "#define %s %s", signature(r, m),
                          macro_body_str(&r->snap->tu.pp, m));
}

/* The parameter list of decl d from its declarations' text: its definition,
 * then its declarations, not spelled in a #define body nor pasted (but a
 * system header's), then through its typedef type, at most 4 steps.  *src,
 * *sf (optional): the event and file whose text gave the list. */
static bool decl_params(Req *r, uint32_t d, CSig *s, const CIdxEvent **src,
                        SrcFile **sf)
{
    const CIndex *ix = r->cidx;
    size_t base = s->label.len;
    int step, pass;
    uint32_t j;
    for (step = 0; step < 4; step++) {
        for (pass = 0; pass < 2; pass++)
            for (j = ix->by_decl_start[d]; j < ix->by_decl_start[d + 1]; j++) {
                const CIdxEvent *e = &ix->ev[ix->by_decl[j]];
                SrcFile *f;
                if ((e->flags & CIX_ROLE) != (pass ? CIX_DECL : CIX_DEF) ||
                    (e->flags & CIX_MACRO_BODY) ||
                    (e->flags & (CIX_AT_EXPANSION | CIX_SYSTEM)) ==
                        CIX_AT_EXPANSION ||
                    ix->files[e->file].stale ||
                    !(f = cindex_srcfile(&r->snap->tu.sm,
                                         ix->files[e->file].path)))
                    continue;
                s->label.len = base;
                if (csig_params(f->buf, f->size, e->off + e->len, s)) {
                    if (src)
                        *src = e;
                    if (sf)
                        *sf = f;
                    return true;
                }
            }
        if (!ix->decls[d].type || ix->decls[ix->decls[d].type - 1].kind != CIK_TYPEDEF)
            break;
        d = ix->decls[d].type - 1;
    }
    s->label.len = base;
    return false;
}

/* The C entity called by the name text[0..n) at offset at: the decls there,
 * else the one of that name visible there; its signature into s. */
static bool c_sig(Req *r, const char *name, size_t n, size_t at, Sig *s)
{
    uint32_t decls[16], *vis = NULL;
    size_t nd = 0, i;
    bool ok = false;
    if (c_file(r) < 0 || at > r->file->size)
        return false;
    nd = cindex_decls_at(r->cidx, r->file->path, (uint32_t)at, decls, 16);
    if (!nd) {
        size_t nv = cindex_visible(r->cidx, r->file->path, (uint32_t)at, &vis);
        for (i = 0; i < nv && !nd; i++) {
            const char *v = cindex_name(r->cidx, vis[i]);
            if (strlen(v) == n && !memcmp(v, name, n))
                decls[nd++] = vis[i];
        }
        free(vis);
    }
    for (i = 0; i < nd && !ok; i++) {
        sb_putn(&s->c.label, name, n);
        if ((ok = decl_params(r, decls[i], &s->c, NULL, NULL)))
            s->doc = r->cidx->strings + r->cidx->decls[decls[i]].hover;
        else
            s->c.label.len = 0;
    }
    return ok;
}

void lsp_signature_help(Req *r, JsonWriter *w)
{
    const JsonValue *pos = json_get(r->params, "position");
    size_t cur = pos_to_offset(r->text, r->text_len,
                               (uint32_t)json_int_of(json_get(pos, "line"), 0),
                               (uint32_t)json_int_of(json_get(pos, "character"), 0),
                               r->enc);
    size_t p = cur, stop = cur > 8192 ? cur - 8192 : 0, ne;
    int depth = 0, commas = 0;
    Ident *id;
    Macro *m;
    Sig sig;
    size_t i;
    int k;
    const char *t = r->text;
    memset(&sig, 0, sizeof sig);
    while (p > stop) {
        char c = t[--p];
        if (c == '"' || c == '\'') { /* skip back over a literal */
            while (p > stop && !(t[p - 1] == c && (p < 2 || t[p - 2] != '\\')))
                p--;
            if (p > stop)
                p--;
        } else if (c == ')') {
            depth++;
        } else if (c == '(') {
            if (depth == 0)
                break;
            depth--;
        } else if (c == ',' && depth == 0) {
            commas++;
        } else if (c == ';' || c == '{' || c == '}') {
            json_null(w);
            return;
        }
    }
    if (p <= stop && !(p == stop && t[p] == '(')) {
        json_null(w);
        return;
    }
    ne = p;
    while (ne > 0 && (t[ne - 1] == ' ' || t[ne - 1] == '\t'))
        ne--;
    p = ne;
    while (p > 0 && ((t[p - 1] >= 'a' && t[p - 1] <= 'z') ||
                     (t[p - 1] >= 'A' && t[p - 1] <= 'Z') ||
                     (t[p - 1] >= '0' && t[p - 1] <= '9') || t[p - 1] == '_'))
        p--;
    if (p == ne) {
        json_null(w);
        return;
    }
    id = intern_find(r->snap->tu.pp.in, t + p, ne - p);
    m = !id ? NULL
            : macro_at_version(r->snap->tu.pp.mt, id,
                               index_seq_at(&r->snap->ix,
                                            r->file->base +
                                                (SrcLoc)(p < r->file->size
                                                             ? p : r->file->size)));
    sig.c.offs.len = 0;
    if (m && m->funclike) {
        macro_sig(r, m, &sig);
    } else if (!c_sig(r, t + p, ne - p, p, &sig)) {
        /* not defined there (or the snapshot is behind): any version */
        m = id ? mt_cur(r->snap->tu.pp.mt, id) : NULL;
        if (!m || !m->funclike) {
            json_null(w);
            sig_free(&sig);
            return;
        }
        macro_sig(r, m, &sig);
    }
    k = (int)(sig.c.offs.len / 2);
    json_begin_object(w);
    json_key(w, "signatures");
    json_begin_array(w);
    json_begin_object(w);
    json_key(w, "parameters");
    json_begin_array(w);
    for (i = 0; i < sig.c.offs.len; i += 2) {
        json_begin_object(w);
        json_key(w, "label");
        json_begin_array(w);
        json_int(w, (long long)sig.c.offs.data[i]);
        json_int(w, (long long)sig.c.offs.data[i + 1]);
        json_end_array(w);
        json_end_object(w);
    }
    json_end_array(w);
    json_key(w, "label");
    json_str(w, sb_cstr(&sig.c.label));
    json_key(w, "documentation");
    json_str(w, sig.doc);
    json_end_object(w);
    json_end_array(w);
    json_key(w, "activeSignature");
    json_int(w, 0);
    json_key(w, "activeParameter");
    json_int(w, k == 0 ? 0 : commas < k ? commas : sig.c.variadic ? k - 1 : commas);
    json_end_object(w);
    sig_free(&sig);
}

/* ---- parameter-name inlay hints (docs/B4_DESIGN.md 6) ------------------------- *
 * Each CIX_CALL event spelled in this file's text gets `NAME:` before every
 * argument, the names from the callee's declaration (decl_params) and the
 * arguments re-lexed from the snapshot's text after the callee's name. */

typedef struct HintName {
    const char *p;
    size_t n;                /* 0: unnamed */
} HintName;

typedef struct HintFn {      /* a callee's parameters, per request */
    uint32_t decl;
    size_t np;               /* 0: no prototype */
    HintName *names;
    bool variadic;
} HintFn;

typedef struct HintArg {
    size_t b, e;             /* the argument's text */
    size_t cb, ce;           /* the comment right before it (cb == ce: none) */
} HintArg;

typedef VEC(HintFn) HintFns;
typedef VEC(HintArg) HintArgs;

/* The names of the parameters of decl d; index into *cache. */
static size_t hint_fn(Req *r, HintFns *cache, uint32_t d)
{
    HintFn f = {0};
    CSig sig = {0};
    const CIdxEvent *src = NULL;
    SrcFile *sf = NULL;
    size_t i;
    for (i = 0; i < cache->len; i++)
        if (cache->data[i].decl == d)
            return i;
    f.decl = d;
    if (decl_params(r, d, &sig, &src, &sf)) {
        f.np = sig.offs.len / 2;
        f.variadic = sig.variadic;
        f.names = NEW_ARRAY(r->arena, HintName, f.np);
        for (i = 0; i < f.np; i++)
            f.names[i].n = cindex_param_name(
                r->cidx, src->file, sf->buf, sig.spans.data[2 * i],
                sig.spans.data[2 * i + 1], (src->flags & CIX_SYSTEM) != 0,
                &f.names[i].p);
    }
    csig_free(&sig);
    vec_push(cache, f);
    return cache->len - 1;
}

static bool blank_ch(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/* buf[i] is a '#' that starts a line. */
static bool line_hash(const char *buf, size_t i)
{
    if (buf[i] != '#')
        return false;
    while (i > 0 && buf[i - 1] != '\n' && blank_ch(buf[i - 1]))
        i--;
    return i == 0 || buf[i - 1] == '\n';
}

/* The arguments of the call whose callee's name ends at buf[i]: past blanks,
 * comments and ')' a '(' must follow.  False: no hints for this call (no '(',
 * a directive in the list, no end within 64 KiB). */
static bool call_args(const char *buf, size_t n, size_t i, HintArgs *out)
{
    size_t lim;
    int depth = 0;
    for (;;) {
        while (i < n && (blank_ch(buf[i]) || buf[i] == '/')) {
            if (buf[i] == '/' && i + 1 < n && buf[i + 1] == '*') {
                for (i += 2; i + 1 < n && !(buf[i] == '*' && buf[i + 1] == '/'); i++)
                    ;
                i += 2;
            } else if (buf[i] == '/' && i + 1 < n && buf[i + 1] == '/') {
                while (i < n && buf[i] != '\n')
                    i++;
            } else if (buf[i] == '/') {
                return false;
            } else {
                i++;
            }
        }
        if (i < n && buf[i] == ')')
            i++;
        else
            break;
    }
    if (i >= n || buf[i] != '(')
        return false;
    out->len = 0;
    lim = n - i > 65536 ? i + 65536 : n;
    for (i++; i < lim;) {
        HintArg a = {0};
        char c;
        for (;;) {           /* blanks and comments; the last comment is kept */
            while (i < lim && blank_ch(buf[i]))
                i++;
            if (i + 1 < lim && buf[i] == '/' && buf[i + 1] == '*') {
                a.cb = i + 2;
                for (i += 2; i + 1 < lim && !(buf[i] == '*' && buf[i + 1] == '/'); i++)
                    ;
                a.ce = i;
                i += 2;
            } else if (i + 1 < lim && buf[i] == '/' && buf[i + 1] == '/') {
                while (i < lim && buf[i] != '\n')
                    i++;
                a.cb = a.ce = 0;
            } else {
                break;
            }
        }
        if (i >= lim)
            return false;
        a.b = a.e = i;
        if (line_hash(buf, i))
            return false;
        for (depth = 0; i < lim; i++) {
            c = buf[i];
            if (c == '"' || c == '\'') {
                for (i++; i < lim && buf[i] != c && buf[i] != '\n'; i++)
                    if (buf[i] == '\\')
                        i++;
            } else if (c == '/' && i + 1 < lim && buf[i + 1] == '*') {
                for (i += 2; i + 1 < lim && !(buf[i] == '*' && buf[i + 1] == '/'); i++)
                    ;
                i++;
                continue;
            } else if (c == '/' && i + 1 < lim && buf[i + 1] == '/') {
                while (i < lim && buf[i] != '\n')
                    i++;
                continue;
            } else if (c == '#' && line_hash(buf, i)) {
                return false;
            } else if (c == '(' || c == '[' || c == '{') {
                depth++;
            } else if (depth && (c == ')' || c == ']' || c == '}')) {
                depth--;
            } else if (!depth && (c == ',' || c == ')')) {
                break;
            }
            if (i < lim && !blank_ch(buf[i]))
                a.e = i + 1;
        }
        if (i >= lim)
            return false;
        if (a.b < a.e || buf[i] == ',' || out->len)
            vec_push(out, a);
        if (buf[i++] == ')')
            return true;
    }
    return false;
}

static bool eq_nocase(const char *a, const char *b, size_t n)
{
    for (; n; n--)
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++))
            return false;
    return true;
}

/* A single parameter that the function's name says: set_limit(int limit). */
static bool setter_of(const char *fn, const HintName *p)
{
    size_t n;
    if (!eq_nocase(fn, "set", 3))
        return false;
    fn += 3;
    fn += *fn == '_';
    n = strlen(fn);
    return n == p->n && eq_nocase(fn, p->p, n);
}

/* No hint: the argument is the parameter's name (outer parentheses aside), or
 * a comment right before it names the parameter. */
static bool arg_says_name(const char *buf, const HintArg *a, const HintName *p)
{
    size_t b = a->b, e = a->e, c0 = a->cb, c1 = a->ce;
    while (c0 < c1 && blank_ch(buf[c0]))
        c0++;
    while (c1 > c0 && blank_ch(buf[c1 - 1]))
        c1--;
    if (c1 > c0 && buf[c1 - 1] == '=') {
        c1--;
        while (c1 > c0 && blank_ch(buf[c1 - 1]))
            c1--;
    }
    if (c1 - c0 == p->n && !memcmp(buf + c0, p->p, p->n))
        return true;
    while (e - b >= 2 && buf[b] == '(' && buf[e - 1] == ')') {
        size_t k;
        int depth = 0;
        for (k = b; k < e; k++) {
            depth += buf[k] == '(';
            depth -= buf[k] == ')';
            if (!depth)
                break;
        }
        if (k != e - 1)
            break;
        for (b++, e--; b < e && blank_ch(buf[b]); b++)
            ;
        while (e > b && blank_ch(buf[e - 1]))
            e--;
    }
    while (e - b > 1 && buf[b] == '_') /* clangd 20 strips both sides (10.2 P1) */
        b++;
    return e - b == p->n && !memcmp(buf + b, p->p, p->n);
}

void lsp_inlay_hints(Req *r, JsonWriter *w)
{
    const JsonValue *rg = json_get(r->params, "range");
    SrcLoc lo = req_loc(r, json_get(rg, "start")), hi = req_loc(r, json_get(rg, "end"));
    const CIndex *cx = r->cidx;
    const char *buf = r->file->buf;
    HintFns cache = {0};
    HintArgs args = {0};
    int fi = c_file(r);
    uint32_t k, s;
    json_begin_array(w);
    if (fi < 0 || hi < lo) {
        json_end_array(w);
        return;
    }
    lo -= r->file->base;
    hi -= r->file->base;
    s = cindex_scope_at(cx, (uint32_t)fi, (uint32_t)lo);
    s = s ? cindex_scope_root(cx, s) : 0;
    for (k = cindex_lower(cx, (uint32_t)fi, s ? cx->scopes[s - 1].begin : (uint32_t)lo);
         k < cx->nev && cx->ev[k].file == (uint32_t)fi && cx->ev[k].off <= hi; k++) {
        const CIdxEvent *ce = &cx->ev[k];
        const HintFn *f;
        size_t i;
        if (!(ce->flags & CIX_CALL) ||
            (ce->flags & (CIX_MACRO_BODY | CIX_AT_EXPANSION | CIX_SYSTEM)) ||
            !call_args(buf, r->file->size, ce->off + ce->len, &args))
            continue;
        i = hint_fn(r, &cache, ce->decl);
        f = &cache.data[i];
        for (i = 0; i < args.len && i < f->np && !(f->variadic && i == f->np - 1); i++) {
            const HintName *p = &f->names[i];
            if (!p->n || args.data[i].b < lo || args.data[i].b > hi ||
                args.data[i].b == args.data[i].e ||
                arg_says_name(buf, &args.data[i], p) ||
                (f->np == 1 && setter_of(cindex_name(cx, ce->decl), p)))
                continue;
            json_begin_object(w);
            json_key(w, "position");
            json_pos(w, &r->snap->tu.sm, r->enc, r->file->base + (SrcLoc)args.data[i].b);
            json_key(w, "label");
            json_str(w, arena_printf(r->arena, "%.*s:", (int)p->n, p->p));
            json_key(w, "kind");
            json_int(w, 2);
            json_key(w, "paddingRight");
            json_bool(w, true);
            json_end_object(w);
        }
    }
    json_end_array(w);
    vec_free(&cache);
    vec_free(&args);
}

/* ---- cereal/expandMacro --------------------------------------------------------- */

void lsp_expand_macro(Req *r, JsonWriter *w)
{
    IdxTarget t = index_resolve(&r->snap->ix, cursor(r));
    if (!t.top) {
        json_null(w);
        return;
    }
    json_begin_object(w);
    json_key(w, "name");
    json_str(w, t.top->e->macro->name->str);
    json_key(w, "range");
    json_range(w, &r->snap->tu.sm, r->enc, t.top->e->name_loc, t.top->e->end_loc);
    json_key(w, "expansion");
    json_str(w, sb_cstr(&t.top->text));
    json_end_object(w);
}

/* ---- notifications --------------------------------------------------------------- */

static int severity(DiagLevel l)
{
    return l >= DL_ERROR ? 1 : l == DL_WARNING ? 2 : l == DL_REMARK ? 3 : 4;
}

/* A diagnostic's range: its own, else the token at its location. */
static SrcLoc diag_end(SrcMgr *sm, const Diagnostic *d)
{
    SrcFile *f;
    const char *p, *end;
    if (d->range.end > d->loc)
        return d->range.end;
    f = srcmgr_file_of(sm, d->loc);
    if (!f)
        return d->loc;
    p = f->buf + (d->loc - f->base);
    end = f->buf + f->size;
    if (p < end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                    *p == '_' || (*p >= '0' && *p <= '9'))) {
        while (p < end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                           *p == '_' || (*p >= '0' && *p <= '9')))
            p++;
    } else if (p < end && *p != '\n') {
        p++;
    }
    return f->base + (SrcLoc)(p - f->buf);
}

static void begin_notification(JsonWriter *w, StrBuf *sb, const char *method)
{
    json_init_buf(w, sb);
    json_begin_object(w);
    json_key(w, "jsonrpc");
    json_str(w, "2.0");
    json_key(w, "method");
    json_str(w, method);
    json_key(w, "params");
    json_begin_object(w);
}

/* Where diagnostic d shows in file f (a file of sm), and its message;
 * false if it does not show there.  Errors in headers that are not open
 * show on the #include that leads to them (outermost first). */
static bool diag_place(SrcMgr *sm, SrcFile *f, const Diagnostic *d,
                       bool (*wanted)(void *ctx, const char *path), void *ctx,
                       Arena *a, SrcLoc *b, SrcLoc *e, const char **msg)
{
    *b = d->loc;
    *msg = d->msg;
    if (in_file(f, *b)) {
        *e = diag_end(sm, d);
        return true;
    }
    {
        SrcFile *df = srcmgr_file_of(sm, *b);
        int j;
        if (d->level < DL_ERROR || !d->ninc || (df && wanted(ctx, df->path)))
            return false;
        for (j = d->ninc - 1; j >= 0 && !in_file(f, d->inc_chain[j]); j--)
            ;
        if (j < 0)
            return false;
        *b = d->inc_chain[j];
        *msg = arena_printf(a, "in included file %s: %s",
                            df ? df->path : "?", d->msg);
        *e = *b;
    }
    return true;
}

static SrcFile *user_file_named(SrcMgr *sm, const char *path)
{
    uint32_t i, n = srcmgr_nfiles(sm);
    for (i = 0; i < n; i++) {
        SrcFile *f = srcmgr_file(sm, i);
        if (f->kind == SF_USER && !strcmp(f->path, path))
            return f;
    }
    return NULL;
}

static int cmp_keys(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Diagnostic d placed at b..e of file f (of sm) with message msg, as offsets
 * in the files' texts; the strings and notes are the arena's.  carry is
 * left false. */
static CDiag cdiag_make(SrcMgr *sm, SrcFile *f, Arena *a, const Diagnostic *d,
                        SrcLoc b, SrcLoc e, const char *msg)
{
    CDiag cd = {0};
    size_t k;
    cd.path = (char *)f->path;
    cd.off_b = b - f->base;
    cd.off_e = (e < b ? b : e) - f->base;
    cd.severity = severity(d->level);
    cd.code = d->id && *d->id ? (char *)d->id : NULL;
    cd.msg = (char *)msg;
    cd.notes = NEW_ARRAY(a, CNote, d->notes.len);
    for (k = 0; k < d->notes.len; k++) {
        SrcLoc nl = d->notes.data[k].loc;
        SrcFile *nf = srcmgr_file_of(sm, nl);
        if (!nf || (nf->kind != SF_USER && nf->kind != SF_SYSTEM))
            continue;
        cd.notes[cd.nnotes].path = (char *)nf->path;
        cd.notes[cd.nnotes].off = nl - nf->base;
        cd.notes[cd.nnotes++].msg = (char *)d->notes.data[k].msg;
    }
    return cd;
}

/* One diagnostic object for file f of sm, whose files hold the offsets of
 * cd (a note into a file sm lacks is left out). */
static void put_diag(JsonWriter *w, SrcMgr *sm, SrcFile *f, PosEncoding enc,
                     Arena *a, const CDiag *cd)
{
    size_t k;
    json_begin_object(w);
    json_key(w, "range");
    json_range(w, sm, enc, f->base + cd->off_b, f->base + cd->off_e);
    json_key(w, "severity");
    json_int(w, cd->severity);
    if (cd->code) {
        json_key(w, "code");
        json_str(w, cd->code);
    }
    json_key(w, "source");
    json_str(w, "cereal");
    json_key(w, "message");
    json_str(w, cd->msg);
    if (cd->nnotes) {
        json_key(w, "relatedInformation");
        json_begin_array(w);
        for (k = 0; k < cd->nnotes; k++) {
            const CNote *n = &cd->notes[k];
            SrcFile *nf = !strcmp(n->path, f->path) ? f
                                                    : cindex_srcfile(sm, n->path);
            SrcLoc nl;
            if (!nf)
                continue;
            nl = nf->base + n->off;
            json_begin_object(w);
            json_key(w, "location");
            json_begin_object(w);
            json_key(w, "uri");
            json_str(w, path_to_uri(a, nf->path));
            json_key(w, "range");
            json_range(w, sm, enc, nl, nl);
            json_end_object(w);
            json_key(w, "message");
            json_str(w, n->msg);
            json_end_object(w);
        }
        json_end_array(w);
    }
    json_end_object(w);
}

static void cdiag_free_fixes(CDiag *cd)
{
    size_t k;
    for (k = 0; k < cd->nfixes; k++) {
        free(cd->fixes[k].path);
        free(cd->fixes[k].text);
    }
    free(cd->fixes);
    cd->fixes = NULL;
    cd->nfixes = 0;
}

/* cd, to keep: its strings copied. */
static CDiag cdiag_dup(const CDiag *cd)
{
    CDiag r = *cd;
    size_t k;
    r.path = xstrdup(cd->path);
    r.code = cd->code ? xstrdup(cd->code) : NULL;
    r.msg = xstrdup(cd->msg);
    r.notes = xcalloc(cd->nnotes + 1, sizeof *r.notes);
    for (k = 0; k < cd->nnotes; k++) {
        r.notes[k].path = xstrdup(cd->notes[k].path);
        r.notes[k].off = cd->notes[k].off;
        r.notes[k].msg = xstrdup(cd->notes[k].msg);
    }
    r.fixes = cd->nfixes ? xcalloc(cd->nfixes, sizeof *r.fixes) : NULL;
    for (k = 0; k < cd->nfixes; k++) {
        r.fixes[k] = cd->fixes[k];
        r.fixes[k].path = xstrdup(cd->fixes[k].path);
        r.fixes[k].text = xstrdup(cd->fixes[k].text);
    }
    return r;
}

static void cdiag_free(CDiag *cd)
{
    size_t k;
    for (k = 0; k < cd->nnotes; k++) {
        free(cd->notes[k].path);
        free(cd->notes[k].msg);
    }
    free(cd->notes);
    cdiag_free_fixes(cd);
    free(cd->path);
    free(cd->code);
    free(cd->msg);
}

/* Whether files of the check have the text of the same path in the
 * snapshot (what it was built from), so that offsets in them are valid
 * there; each file is compared once. */
typedef struct SameText {
    VEC(SrcFile *) yes, no;
} SameText;

static bool file_same(SameText *st, SrcFile *f, SrcMgr *other)
{
    SrcFile *g;
    size_t i;
    for (i = 0; i < st->yes.len; i++)
        if (st->yes.data[i] == f)
            return true;
    for (i = 0; i < st->no.len; i++)
        if (st->no.data[i] == f)
            return false;
    g = cindex_srcfile(other, f->path);
    if (g && g->size == f->size && !memcmp(g->buf, f->buf, f->size)) {
        vec_push(&st->yes, f);
        return true;
    }
    vec_push(&st->no, f);
    return false;
}

/* The fix-its of d (a diagnostic of the check, sm) on cd, if every file they
 * edit has the snapshot's text (sm0): the group is atomic (C_DESIGN.md 7.2). */
static void cdiag_set_fixes(CDiag *cd, SrcMgr *sm, SrcMgr *sm0, SameText *st,
                            Arena *a, const Diagnostic *d)
{
    DiagEditVec ge = {0};
    size_t k;
    (void)sm0;
    if (!diag_fix_group(sm, d, &ge))
        return;
    for (k = 0; k < ge.len; k++)
        if (!file_same(st, ge.data[k].file, sm0)) {
            vec_free(&ge);
            return;
        }
    cd->fixes = NEW_ARRAY(a, CFix, ge.len);
    for (k = 0; k < ge.len; k++) {
        cd->fixes[k].path = (char *)ge.data[k].file->path;
        cd->fixes[k].off_b = ge.data[k].begin;
        cd->fixes[k].off_e = ge.data[k].end;
        cd->fixes[k].text = (char *)ge.data[k].text;
        cd->fixes[k].app = ge.data[k].app;
    }
    cd->nfixes = ge.len;
    vec_free(&ge);
}

static const char *diag_key(SrcMgr *sm, PosEncoding enc, Arena *a, SrcLoc b,
                            SrcLoc e, const char *msg)
{
    uint32_t l0, c0, l1, c1;
    loc_to_pos(sm, b, enc, &l0, &c0);
    loc_to_pos(sm, e < b ? b : e, enc, &l1, &c1);
    return arena_printf(a, "%u:%u-%u:%u|%s", l0, c0, l1, c1, msg);
}

static bool key_seen(const char *key, const char **keys, size_t n)
{
    return n && bsearch(&key, keys, n, sizeof *keys, cmp_keys);
}

void cdiags_free(Snapshot *s)
{
    size_t i;
    for (i = 0; i < s->cdiags.len; i++)
        cdiag_free(&s->cdiags.data[i]);
    vec_free(&s->cdiags);
}

/* The edit of file `path` from a's text to b's, cached by path in ed. */
typedef struct PathEdit {
    const char *path;
    bool present;            /* in both snapshots */
    CIdxEdit e;
} PathEdit;
typedef VEC(PathEdit) PathEdits;

static const PathEdit *path_edit(PathEdits *ed, Snapshot *a, Snapshot *b,
                                 const char *path)
{
    size_t i;
    PathEdit pe = {0};
    SrcFile *fa, *fb;
    for (i = 0; i < ed->len; i++)
        if (!strcmp(ed->data[i].path, path))
            return &ed->data[i];
    pe.path = path;
    fa = cindex_srcfile(&a->tu.sm, path);
    fb = cindex_srcfile(&b->tu.sm, path);
    if (fa && fb) {
        pe.present = true;
        cindex_text_edit(fa->buf, fa->size, fb->buf, fb->size, &pe.e);
    }
    vec_push(ed, pe);
    return &ed->data[ed->len - 1];
}

/* An offset range of file `path` that the edit leaves alone, mapped: false
 * if the file is gone or the range meets the edit's span (inclusive: a
 * cursor at its edge is inside). */
static bool carry_range(PathEdits *ed, Snapshot *a, Snapshot *b,
                        const char *path, uint32_t *lo, uint32_t *hi)
{
    const PathEdit *pe = path_edit(ed, a, b, path);
    if (!pe->present)
        return false;
    if (pe->e.same)
        return true;
    if (*lo <= pe->e.old_end && *hi >= pe->e.pre)
        return false;
    *lo = cindex_edit_map(&pe->e, *lo);
    *hi = cindex_edit_map(&pe->e, *hi);
    return true;
}

void cdiags_carry(Snapshot *to, Snapshot *from)
{
    PathEdits ed = {0};
    size_t i, k;
    for (i = 0; i < from->cdiags.len; i++) {
        const CDiag *c = &from->cdiags.data[i];
        CDiag cd;
        bool ok;
        if (!c->carry)
            continue;
        cd = cdiag_dup(c);
        ok = carry_range(&ed, from, to, c->path, &cd.off_b, &cd.off_e);
        for (k = 0; ok && k < cd.nnotes; k++) {
            uint32_t hi = cd.notes[k].off;
            ok = carry_range(&ed, from, to, c->notes[k].path,
                             &cd.notes[k].off, &hi);
        }
        /* the fixes move like the range; one that meets the edit takes the
         * whole group with it, the diagnostic stays (C_DESIGN.md 7.2) */
        for (k = 0; ok && k < cd.nfixes; k++) {
            uint32_t lo = cd.fixes[k].off_b, hi = cd.fixes[k].off_e;
            if (!carry_range(&ed, from, to, cd.fixes[k].path, &lo, &hi)) {
                cdiag_free_fixes(&cd);
                break;
            }
            cd.fixes[k].off_b = lo;
            cd.fixes[k].off_e = hi;
        }
        if (ok)
            vec_push(&to->cdiags, cd);
        else
            cdiag_free(&cd);
    }
    vec_free(&ed);
}

/* The diagnostics of each open file the snapshot covers: the macro phase's
 * (s->tu), then the compiler's minus those at the same range with the same
 * message: chk's (a TU of the same unit), which become s->cdiags, else the
 * ones carried over in s->cdiags.  The document version is the overlay's
 * (the text both phases read). */
void lsp_publish_diagnostics(Snapshot *s, TU *chk, PosEncoding enc,
                             bool (*wanted)(void *ctx, const char *path),
                             void *ctx)
{
    SrcMgr *sm0 = &s->tu.sm;
    uint32_t fi, nf = srcmgr_nfiles(sm0);
    SameText same = {{0}, {0}};
    Arena a;
    arena_init(&a);
    if (chk)
        cdiags_free(s);
    for (fi = 0; fi < nf; fi++) {
        SrcFile *f0 = srcmgr_file(sm0, fi);
        StrBuf sb = {0};
        JsonWriter w;
        VEC(const char *) seen = {0}; /* keys of the macro phase's */
        bool keyed = chk || s->cdiags.len;
        long long version;
        size_t i;
        if (f0->kind != SF_USER || !wanted(ctx, f0->path))
            continue;
        begin_notification(&w, &sb, "textDocument/publishDiagnostics");
        json_key(&w, "uri");
        json_str(&w, path_to_uri(&a, f0->path));
        if (lsp_overlay_version(s->overlay, f0->path, &version)) {
            json_key(&w, "version");
            json_int(&w, version);
        }
        json_key(&w, "diagnostics");
        json_begin_array(&w);
        for (i = 0; i < s->tu.diag.all.len; i++) {
            Diagnostic *d = s->tu.diag.all.data[i];
            SrcLoc b, e;
            const char *msg;
            CDiag cd;
            if (!diag_place(sm0, f0, d, wanted, ctx, &a, &b, &e, &msg))
                continue;
            if (keyed)
                vec_push(&seen, diag_key(sm0, enc, &a, b, e, msg));
            cd = cdiag_make(sm0, f0, &a, d, b, e, msg);
            put_diag(&w, sm0, f0, enc, &a, &cd);
        }
        if (seen.len)
            qsort(seen.data, seen.len, sizeof *seen.data, cmp_keys);
        if (chk) {
            SrcMgr *sm = &chk->sm;
            SrcFile *f = user_file_named(sm, f0->path);
            for (i = 0; f && i < chk->diag.all.len; i++) {
                Diagnostic *d = chk->diag.all.data[i];
                SrcLoc b, e;
                const char *msg, *key;
                CDiag cd, own;
                size_t k;
                if (!diag_place(sm, f, d, wanted, ctx, &a, &b, &e, &msg))
                    continue;
                key = diag_key(sm, enc, &a, b, e, msg);
                if (key_seen(key, seen.data, seen.len))
                    continue;
                cd = cdiag_make(sm, f, &a, d, b, e, msg);
                put_diag(&w, sm, f, enc, &a, &cd);
                cd.carry = file_same(&same, f, sm0);
                for (k = 0; cd.carry && k < cd.nnotes; k++) {
                    SrcFile *nfile = cindex_srcfile(sm, cd.notes[k].path);
                    cd.carry = nfile && file_same(&same, nfile, sm0);
                }
                cdiag_set_fixes(&cd, sm, sm0, &same, &a, d);
                own = cdiag_dup(&cd);
                vec_push(&s->cdiags, own);
            }
        } else {
            for (i = 0; i < s->cdiags.len; i++) {
                const CDiag *cd = &s->cdiags.data[i];
                if (strcmp(cd->path, f0->path))
                    continue;
                if (!key_seen(diag_key(sm0, enc, &a, f0->base + cd->off_b,
                                       f0->base + cd->off_e, cd->msg),
                              seen.data, seen.len))
                    put_diag(&w, sm0, f0, enc, &a, cd);
            }
        }
        if (s->notice && !strcmp(f0->path, s->main)) {
            json_begin_object(&w);
            json_key(&w, "range");
            json_range(&w, sm0, enc, f0->base, f0->base);
            json_key(&w, "severity");
            json_int(&w, 3);
            json_key(&w, "source");
            json_str(&w, "cereal");
            json_key(&w, "message");
            json_str(&w, s->notice);
            json_end_object(&w);
        }
        vec_free(&seen);
        json_end_array(&w);
        json_end_object(&w);
        json_end_object(&w);
        rpc_write(sb.data, sb.len);
        sb_free(&sb);
    }
    vec_free(&same.yes);
    vec_free(&same.no);
    arena_free(&a);
}

/* ---- code actions (C_DESIGN.md 7.3, 7.4) ------------------------------------------- */

void lsp_cdiags_copy(const Snapshot *s, Req *r)
{
    size_t i;
    for (i = 0; i < s->cdiags.len; i++)
        if (s->cdiags.data[i].nfixes) {
            CDiag c = cdiag_dup(&s->cdiags.data[i]);
            vec_push(&r->cdiags, c);
        }
}

void lsp_cdiags_release(Req *r)
{
    size_t i;
    for (i = 0; i < r->cdiags.len; i++)
        cdiag_free(&r->cdiags.data[i]);
    vec_free(&r->cdiags);
}

typedef struct AFix {
    const char *path;
    uint32_t b, e;
    const char *text;
    uint8_t app;
} AFix;

/* A diagnostic that has a fix group, as shown in the document. */
typedef struct AItem {
    CDiag cd;                /* for put_diag, offsets in the document */
    const char *key;         /* diag_key */
    AFix *fx;
    size_t nfx;
    bool hit;                /* meets the request range */
    bool safe;               /* every fix of the group is SAFE */
} AItem;

static bool never_wanted(void *ctx, const char *path)
{
    (void)ctx;
    (void)path;
    return false;
}

/* Is kind asked for by the request's only list (kinds are hierarchical)? */
static bool kind_wanted(const JsonValue *only, const char *kind)
{
    size_t i, n = strlen(kind);
    if (!only || only->kind != JV_ARR)
        return true;
    for (i = 0; i < only->len; i++) {
        const char *o = json_str_of(only->items[i], "");
        size_t l = strlen(o);
        if (l <= n && !strncmp(kind, o, l) && (l == n || kind[l] == '.' || !l))
            return true;
    }
    return false;
}

/* s cut for a title: one line, at most 32 bytes. */
static const char *brief(Arena *a, const char *s, size_t n)
{
    size_t k = 0;
    while (k < n && s[k] != '\n' && k < 32)
        k++;
    return arena_printf(a, "%.*s%s", (int)k, s, k < n ? "..." : "");
}

static const char *fix_title(Req *r, SrcMgr *sm0, Arena *a, const AFix *fx,
                             size_t n)
{
    size_t tl;
    if (n != 1)
        return "Apply fix-it";
    tl = strlen(fx->text);
    if (fx->b == fx->e) {
        if (!strncmp(fx->text, "#include ", 9)) {
            while (tl && (fx->text[tl - 1] == '\n' || fx->text[tl - 1] == '\r'))
                tl--;
            return arena_printf(a, "Add %.*s", (int)tl, fx->text);
        }
        return arena_printf(a, "Insert '%s'", brief(a, fx->text, tl));
    }
    if (!tl) {
        SrcFile *f = !strcmp(fx->path, r->file->path) ? r->file
                                                      : cindex_srcfile(sm0, fx->path);
        if (f && fx->e <= f->size)
            return arena_printf(a, "Remove '%s'",
                                brief(a, f->buf + fx->b, fx->e - fx->b));
        return "Apply fix-it";
    }
    return arena_printf(a, "Replace with '%s'", brief(a, fx->text, tl));
}

/* A WorkspaceEdit of the fixes: documentChanges with the snapshot's version of
 * each document if the client takes them, else changes (as rename). */
static void put_fix_edit(JsonWriter *w, Req *r, const AFix *fx, size_t n)
{
    SrcMgr *sm0 = &r->snap->tu.sm;
    const char **paths = NEW_ARRAY(r->arena, const char *, n + 1);
    size_t np = 0, i, p;
    for (i = 0; i < n; i++) {
        for (p = 0; p < np && strcmp(paths[p], fx[i].path); p++)
            ;
        if (p == np)
            paths[np++] = fx[i].path;
    }
    json_begin_object(w);
    json_key(w, r->doc_changes ? "documentChanges" : "changes");
    if (r->doc_changes)
        json_begin_array(w);
    else
        json_begin_object(w);
    for (p = 0; p < np; p++) {
        SrcFile *f = !strcmp(paths[p], r->file->path) ? r->file
                                                      : cindex_srcfile(sm0, paths[p]);
        long long ver;
        if (!f)
            continue;
        if (r->doc_changes) {
            json_begin_object(w);
            json_key(w, "textDocument");
            json_begin_object(w);
            json_key(w, "uri");
            json_str(w, path_to_uri(r->arena, f->path));
            json_key(w, "version");
            if (lsp_overlay_version(r->snap->overlay, f->path, &ver))
                json_int(w, ver);
            else
                json_null(w);
            json_end_object(w);
            json_key(w, "edits");
        } else {
            json_key(w, path_to_uri(r->arena, f->path));
        }
        json_begin_array(w);
        for (i = 0; i < n; i++) {
            if (strcmp(fx[i].path, paths[p]))
                continue;
            json_begin_object(w);
            json_key(w, "range");
            json_range(w, sm0, r->enc, f->base + fx[i].b, f->base + fx[i].e);
            json_key(w, "newText");
            json_str(w, fx[i].text);
            json_end_object(w);
        }
        json_end_array(w);
        if (r->doc_changes)
            json_end_object(w);
    }
    if (r->doc_changes)
        json_end_array(w);
    else
        json_end_object(w);
    json_end_object(w);
}

/* The client's own diagnostic with this key (range and message), or NULL. */
static const JsonValue *client_diag(Req *r, const char *key)
{
    const JsonValue *ds = json_path(r->params, "context.diagnostics");
    size_t i;
    for (i = 0; ds && ds->kind == JV_ARR && i < ds->len; i++) {
        const JsonValue *d = ds->items[i], *s = json_path(d, "range.start"),
                        *e = json_path(d, "range.end");
        const char *k = arena_printf(
            r->arena, "%u:%u-%u:%u|%s",
            (unsigned)json_int_of(json_get(s, "line"), 0),
            (unsigned)json_int_of(json_get(s, "character"), 0),
            (unsigned)json_int_of(json_get(e, "line"), 0),
            (unsigned)json_int_of(json_get(e, "character"), 0),
            json_str_of(json_get(d, "message"), ""));
        if (!strcmp(k, key))
            return d;
    }
    return NULL;
}

void lsp_code_action(Req *r, JsonWriter *w)
{
    SrcMgr *sm0 = &r->snap->tu.sm;
    SrcFile *f0 = r->file;
    Arena *a = r->arena;
    const JsonValue *only = json_path(r->params, "context.only");
    const JsonValue *rg = json_get(r->params, "range");
    SrcLoc rb, re;
    VEC(AItem) items = {0};
    VEC(const char *) seen = {0};   /* keys of the macro phase's diagnostics */
    DiagEditVec ge = {0};
    bool any_safe = false;
    size_t i, k;
    json_begin_array(w);
    if (r->stale || !rg) {
        json_end_array(w);
        return;
    }
    rb = req_loc(r, json_get(rg, "start"));
    re = req_loc(r, json_get(rg, "end"));
    if (re < rb)
        re = rb;
    for (i = 0; i < r->snap->tu.diag.all.len; i++) {
        Diagnostic *d = r->snap->tu.diag.all.data[i];
        SrcLoc b, e;
        const char *msg;
        AItem it;
        if (!d || d->level == DL_IGNORED ||
            !diag_place(sm0, f0, d, never_wanted, NULL, a, &b, &e, &msg))
            continue;
        vec_push(&seen, diag_key(sm0, r->enc, a, b, e, msg));
        ge.len = 0;
        if (!diag_fix_group(sm0, d, &ge))
            continue;
        memset(&it, 0, sizeof it);
        it.cd = cdiag_make(sm0, f0, a, d, b, e, msg);
        it.key = seen.data[seen.len - 1];
        it.fx = NEW_ARRAY(a, AFix, ge.len);
        it.safe = true;
        for (k = 0; k < ge.len; k++) {
            it.fx[k].path = ge.data[k].file->path;
            it.fx[k].b = ge.data[k].begin;
            it.fx[k].e = ge.data[k].end;
            it.fx[k].text = arena_strdup(a, ge.data[k].text);
            it.fx[k].app = ge.data[k].app;
            it.safe &= ge.data[k].app == FIX_SAFE;
        }
        it.nfx = ge.len;
        it.hit = b <= re && (e < b ? b : e) >= rb;
        vec_push(&items, it);
    }
    vec_free(&ge);
    for (i = 0; i < r->cdiags.len; i++) {
        const CDiag *c = &r->cdiags.data[i];
        AItem it;
        size_t j;
        bool dup = false;
        if (strcmp(c->path, f0->path) || !c->nfixes)
            continue;
        memset(&it, 0, sizeof it);
        it.cd = *c;
        it.key = diag_key(sm0, r->enc, a, f0->base + c->off_b, f0->base + c->off_e,
                          c->msg);
        for (j = 0; j < seen.len && !dup; j++)
            dup = !strcmp(seen.data[j], it.key);
        if (dup)
            continue;
        it.fx = NEW_ARRAY(a, AFix, c->nfixes);
        it.safe = true;
        for (k = 0; k < c->nfixes; k++) {
            it.fx[k].path = c->fixes[k].path;
            it.fx[k].b = c->fixes[k].off_b;
            it.fx[k].e = c->fixes[k].off_e;
            it.fx[k].text = c->fixes[k].text;
            it.fx[k].app = c->fixes[k].app;
            it.safe &= c->fixes[k].app == FIX_SAFE;
        }
        it.nfx = c->nfixes;
        it.hit = f0->base + c->off_b <= re && f0->base + c->off_e >= rb;
        vec_push(&items, it);
    }
    for (i = 0; i < items.len; i++)
        any_safe |= items.data[i].safe;
    if (kind_wanted(only, "quickfix"))
        for (i = 0; i < items.len; i++) {
            const AItem *it = &items.data[i];
            const JsonValue *cl;
            if (!it->hit)
                continue;
            json_begin_object(w);
            json_key(w, "title");
            json_str(w, fix_title(r, sm0, a, it->fx, it->nfx));
            json_key(w, "kind");
            json_str(w, "quickfix");
            json_key(w, "diagnostics");
            json_begin_array(w);
            if ((cl = client_diag(r, it->key)) != NULL)
                json_raw(w, cl->text, cl->text_len);
            else
                put_diag(w, sm0, f0, r->enc, a, &it->cd);
            json_end_array(w);
            if (it->safe) {
                json_key(w, "isPreferred");
                json_bool(w, true);
            }
            json_key(w, "edit");
            put_fix_edit(w, r, it->fx, it->nfx);
            json_end_object(w);
        }
    if (kind_wanted(only, "source.fixAll.cereal") &&
        ((only && only->kind == JV_ARR) || any_safe)) {
        const FixTextEdits *fe = lsp_fix_all(r);
        if (fe && fe->len) {
            AFix *fx = NEW_ARRAY(a, AFix, fe->len);
            for (k = 0; k < fe->len; k++) {
                fx[k].path = f0->path;
                fx[k].b = fe->data[k].begin;
                fx[k].e = fe->data[k].end;
                fx[k].text = fe->data[k].text;
                fx[k].app = FIX_SAFE;
            }
            json_begin_object(w);
            json_key(w, "title");
            json_str(w, "Fix all auto-fixable problems");
            json_key(w, "kind");
            json_str(w, "source.fixAll.cereal");
            json_key(w, "edit");
            put_fix_edit(w, r, fx, fe->len);
            json_end_object(w);
        }
    }
    vec_free(&items);
    vec_free(&seen);
    json_end_array(w);
}

void lsp_publish_inactive(Snapshot *s, PosEncoding enc, const char *path)
{
    Index *ix = &s->ix;
    SrcFile *f = index_find_file(ix, path);
    StrBuf sb = {0};
    JsonWriter w;
    Arena a;
    size_t i;
    if (!f)
        return;
    arena_init(&a);
    begin_notification(&w, &sb, "textDocument/inactiveRegions");
    json_key(&w, "textDocument");
    json_begin_object(&w);
    json_key(&w, "uri");
    json_str(&w, path_to_uri(&a, f->path));
    json_end_object(&w);
    json_key(&w, "regions");
    json_begin_array(&w);
    for (i = 0; i < ix->inactive.len; i++)
        if (in_file(f, ix->inactive.data[i].begin))
            json_range(&w, &s->tu.sm, enc, ix->inactive.data[i].begin,
                       ix->inactive.data[i].end);
    json_end_array(&w);
    json_end_object(&w);
    json_end_object(&w);
    rpc_write(sb.data, sb.len);
    sb_free(&sb);
    arena_free(&a);
}
