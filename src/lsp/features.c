/* features.c - LSP requests answered from a snapshot's index.
 *
 * Macros get the features variables and functions get elsewhere:
 * definition, references, hover, completion, document symbols, semantic
 * tokens, folding, rename, call hierarchy and signature help, plus
 * diagnostics, inactive regions and a macro expansion view. */
#include "lsp.h"

#include <string.h>

/* ---- helpers ------------------------------------------------------------ */

SrcLoc req_loc(Req *r, const JsonValue *pos)
{
    size_t off = pos_to_offset(r->file->buf, r->file->size,
                               (uint32_t)json_int_of(json_get(pos, "line"), 0),
                               (uint32_t)json_int_of(json_get(pos, "character"), 0),
                               r->enc);
    return r->file->base + (SrcLoc)off;
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
    json_begin_object(w);
    json_key(w, "uri");
    json_str(w, uri_of(r->arena, &r->snap->tu.sm, b));
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

void lsp_definition(Req *r, JsonWriter *w)
{
    IdxTarget t = index_resolve(&r->snap->ix, cursor(r));
    SrcMgr *sm = &r->snap->tu.sm;
    int k;
    json_begin_array(w);
    if (t.kind == TGT_MACRO) {
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

void lsp_references(Req *r, JsonWriter *w)
{
    IdxTarget t = index_resolve(&r->snap->ix, cursor(r));
    IdxRef *refs;
    size_t n, i, ndef;
    bool decl = json_bool_of(json_path(r->params, "context.includeDeclaration"),
                             true);
    json_begin_array(w);
    if (t.kind == TGT_MACRO || t.kind == TGT_PARAM) {
        n = index_references(&r->snap->ix, &t, &refs);
        ndef = t.kind == TGT_PARAM ? 1 : (size_t)t.nmacros;
        for (i = 0; i < n; i++) {
            if ((i < ndef && !decl) || !real_loc(&r->snap->tu.sm, refs[i].loc))
                continue;
            json_location(w, r, refs[i].loc, refs[i].loc + refs[i].len);
        }
    }
    json_end_array(w);
}

void lsp_hover(Req *r, JsonWriter *w)
{
    IdxTarget t = index_resolve(&r->snap->ix, cursor(r));
    StrBuf sb = {0};
    int k;
    if (t.kind == TGT_NONE) {
        json_null(w);
        return;
    }
    if (t.kind == TGT_PARAM) {
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
                sb_printf(&sb, "defined at %s:%u", f->name, l);
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

enum { CIK_FUNCTION = 3, CIK_CONSTANT = 21 };
enum { SK_FUNCTION = 12, SK_CONSTANT = 14 };

void lsp_completion(Req *r, JsonWriter *w)
{
    Macro **v;
    size_t n = index_visible(&r->snap->ix, cursor(r), &v), i;
    json_begin_object(w);
    json_key(w, "isIncomplete");
    json_bool(w, false);
    json_key(w, "items");
    json_begin_array(w);
    for (i = 0; i < n; i++) {
        Macro *m = v[i];
        json_begin_object(w);
        json_key(w, "label");
        json_str(w, m->name->str);
        json_key(w, "kind");
        json_int(w, m->funclike ? CIK_FUNCTION : CIK_CONSTANT);
        json_key(w, "detail");
        json_str(w, arena_printf(r->arena, "#define %s %s", signature(r, m),
                                 macro_body_str(&r->snap->tu.pp, m)));
        json_end_object(w);
    }
    json_end_array(w);
    json_end_object(w);
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
    json_end_array(w);
}

/* ---- semantic tokens -------------------------------------------------------- */

enum { ST_MACRO = 0, ST_PARAM = 1 };
enum { SM_DECLARATION = 1 };

typedef struct STok {
    SrcLoc loc;
    uint32_t len;
    int type, mods;
} STok;

static int stok_cmp(const void *a, const void *b)
{
    const STok *x = a, *y = b;
    if (x->loc != y->loc)
        return x->loc < y->loc ? -1 : 1;
    return y->mods - x->mods; /* the declaration first at a tie */
}

void lsp_semantic_tokens(Req *r, JsonWriter *w)
{
    Index *ix = &r->snap->ix;
    PP *pp = &r->snap->tu.pp;
    VEC(STok) v = {0};
    size_t i;
    SrcLoc prev_end = 0;
    uint32_t pl = 0, pc = 0;
    for (i = 0; i < ix->refs.len; i++) {
        IdxRef *ref = &ix->refs.data[i];
        STok s;
        if (!ref->len || !in_file(r->file, ref->loc))
            continue;
        s.loc = ref->loc;
        s.len = ref->len;
        s.type = ST_MACRO;
        s.mods = 0;
        vec_push(&v, s);
    }
    for (i = 0; i < ix->params.len; i++) {
        IdxParamRef *p = &ix->params.data[i];
        STok s;
        if (!in_file(r->file, p->loc))
            continue;
        s.loc = p->loc;
        s.len = p->len;
        s.type = ST_PARAM;
        s.mods = 0;
        vec_push(&v, s);
    }
    for (i = 0; i < pp->macros.len; i++) {
        Macro *m = pp->macros.data[i];
        STok s;
        int k;
        if (m->alias_of || !in_file(r->file, m->name_loc) || !m->name_loc)
            continue;
        s.loc = m->name_loc;
        s.len = m->name->len;
        s.type = ST_MACRO;
        s.mods = SM_DECLARATION;
        vec_push(&v, s);
        for (k = 0; k < m->nparams; k++) {
            if (!m->param_locs[k])
                continue;
            s.loc = m->param_locs[k];
            s.len = m->params[k]->len;
            s.type = ST_PARAM;
            vec_push(&v, s);
        }
    }
    if (v.len)
        qsort(v.data, v.len, sizeof *v.data, stok_cmp);
    json_begin_object(w);
    json_key(w, "data");
    json_begin_array(w);
    for (i = 0; i < v.len; i++) {
        STok *s = &v.data[i];
        uint32_t l, c, el, ec;
        if (s->loc < prev_end)
            continue; /* overlapping or duplicate */
        loc_to_pos(&r->snap->tu.sm, s->loc, r->enc, &l, &c);
        loc_to_pos(&r->snap->tu.sm, s->loc + s->len, r->enc, &el, &ec);
        if (el != l)
            continue; /* tokens are single-line */
        json_int(w, l - pl);
        json_int(w, l == pl ? c - pc : c);
        json_int(w, ec - c);
        json_int(w, s->type);
        json_int(w, s->mods);
        pl = l;
        pc = c;
        prev_end = s->loc + s->len;
    }
    json_end_array(w);
    json_end_object(w);
    vec_free(&v);
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
                                f ? f->name : "?", l);
        }
        if (!real_loc(sm, refs[i].loc))
            continue;
        if (srcmgr_file_of(sm, refs[i].loc)->system_header)
            return "a reference is in a system header";
    }
    return NULL;
}

void lsp_prepare_rename(Req *r, JsonWriter *w)
{
    IdxTarget t = index_resolve(&r->snap->ix, cursor(r));
    IdxRef *refs = NULL;
    size_t n = 0;
    if (t.kind == TGT_MACRO || t.kind == TGT_PARAM)
        n = index_references(&r->snap->ix, &t, &refs);
    if (rename_blocker(r, &t, refs, n) || !t.range.end) {
        json_null(w);
        return;
    }
    json_begin_object(w);
    json_key(w, "range");
    json_range(w, &r->snap->tu.sm, r->enc, t.range.begin, t.range.end);
    json_key(w, "placeholder");
    json_str(w, t.name->str);
    json_end_object(w);
}

static bool is_identifier(const char *s)
{
    if (!*s || (*s >= '0' && *s <= '9'))
        return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || *s == '_'))
            return false;
    return true;
}

static int ref_file_cmp(const void *a, const void *b)
{
    const IdxRef *x = a, *y = b;
    return x->loc < y->loc ? -1 : x->loc > y->loc;
}

bool lsp_rename(Req *r, JsonWriter *w, const char **err)
{
    IdxTarget t = index_resolve(&r->snap->ix, cursor(r));
    const char *name = json_str_of(json_get(r->params, "newName"), "");
    SrcMgr *sm = &r->snap->tu.sm;
    IdxRef *refs = NULL, *sorted;
    size_t n = 0, i;
    SrcFile *cur = NULL;
    SrcLoc last = 0;
    if (t.kind == TGT_MACRO || t.kind == TGT_PARAM)
        n = index_references(&r->snap->ix, &t, &refs);
    if ((*err = rename_blocker(r, &t, refs, n)) != NULL)
        return false;
    if (!is_identifier(name)) {
        *err = "the new name is not an identifier";
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

/* ---- call hierarchy --------------------------------------------------------- */

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
    json_end_object(w);
    json_end_object(w);
}

void lsp_prepare_call_hierarchy(Req *r, JsonWriter *w)
{
    IdxTarget t = index_resolve(&r->snap->ix, cursor(r));
    int k;
    if (t.kind != TGT_MACRO || !t.nmacros) {
        json_null(w);
        return;
    }
    json_begin_array(w);
    for (k = 0; k < t.nmacros; k++)
        if (real_loc(&r->snap->tu.sm, t.macros[k]->name_loc))
            call_item(r, w, t.macros[k]);
    json_end_array(w);
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
    if (id >= 0 && (size_t)id < pp->macros.len &&
        !strcmp(pp->macros.data[id]->name->str, name))
        return pp->macros.data[id];
    if (sel) {
        SrcLoc loc = req_loc(r, sel);
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

void lsp_calls(Req *r, JsonWriter *w, bool incoming)
{
    Macro *m = item_macro(r);
    IdxCall *calls;
    size_t n, i;
    if (!m) {
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
 * name before it, counting top-level commas on the way. */

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
    StrBuf label = {0};
    int k;
    const char *t = r->text;
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
    if (!id) {
        json_null(w);
        return;
    }
    m = macro_at_version(r->snap->tu.pp.mt, id, index_seq_at(&r->snap->ix,
                                          r->file->base +
                                              (SrcLoc)(p < r->file->size
                                                           ? p : r->file->size)));
    if (!m) /* not defined there (or the snapshot is behind): any version */
        m = mt_cur(r->snap->tu.pp.mt, id);
    if (!m || !m->funclike) {
        json_null(w);
        return;
    }
    sb_printf(&label, "%s(", m->name->str);
    json_begin_object(w);
    json_key(w, "signatures");
    json_begin_array(w);
    json_begin_object(w);
    json_key(w, "parameters");
    json_begin_array(w);
    for (k = 0; k < m->nparams; k++) {
        const char *pn = m->variadic && k == m->nparams - 1 &&
                                 !m->gnu_named_variadic
                             ? "..." : m->params[k]->str;
        size_t b;
        if (k)
            sb_puts(&label, ", ");
        b = label.len;
        sb_puts(&label, pn);
        if (m->gnu_named_variadic && k == m->nparams - 1)
            sb_puts(&label, "...");
        json_begin_object(w);
        json_key(w, "label");
        json_begin_array(w);
        json_int(w, (long long)b);
        json_int(w, (long long)label.len);
        json_end_array(w);
        json_end_object(w);
    }
    sb_putc(&label, ')');
    json_end_array(w);
    json_key(w, "label");
    json_str(w, sb_cstr(&label));
    json_key(w, "documentation");
    json_str(w, arena_printf(r->arena, "#define %s %s", signature(r, m),
                             macro_body_str(&r->snap->tu.pp, m)));
    json_end_object(w);
    json_end_array(w);
    json_key(w, "activeSignature");
    json_int(w, 0);
    json_key(w, "activeParameter");
    json_int(w, m->nparams == 0 ? 0
                : commas < m->nparams ? commas
                : m->variadic ? m->nparams - 1 : commas);
    json_end_object(w);
    sb_free(&label);
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

void lsp_publish_diagnostics(Snapshot *s, PosEncoding enc,
                             bool (*wanted)(void *ctx, const char *path),
                             void *ctx)
{
    SrcMgr *sm = &s->tu.sm;
    uint32_t fi, nf = srcmgr_nfiles(sm);
    Arena a;
    arena_init(&a);
    for (fi = 0; fi < nf; fi++) {
        SrcFile *f = srcmgr_file(sm, fi);
        StrBuf sb = {0};
        JsonWriter w;
        size_t i;
        if (f->kind != SF_USER || !wanted(ctx, f->path))
            continue;
        begin_notification(&w, &sb, "textDocument/publishDiagnostics");
        json_key(&w, "uri");
        json_str(&w, path_to_uri(&a, f->path));
        json_key(&w, "diagnostics");
        json_begin_array(&w);
        for (i = 0; i < s->tu.diag.all.len; i++) {
            Diagnostic *d = s->tu.diag.all.data[i];
            SrcLoc b = d->loc, e;
            const char *msg = d->msg;
            size_t k;
            if (!in_file(f, b)) {
                /* errors in headers that are not open show on the
                 * #include that leads to them (outermost first) */
                SrcFile *df = srcmgr_file_of(sm, b);
                int j;
                if (d->level < DL_ERROR || !d->ninc || (df && wanted(ctx, df->path)))
                    continue;
                for (j = d->ninc - 1; j >= 0 && !in_file(f, d->inc_chain[j]); j--)
                    ;
                if (j < 0)
                    continue;
                b = d->inc_chain[j];
                msg = arena_printf(&a, "in included file %s: %s",
                                   df ? df->name : "?", d->msg);
                e = b;
            } else {
                e = diag_end(sm, d);
            }
            json_begin_object(&w);
            json_key(&w, "range");
            json_range(&w, sm, enc, b, e);
            json_key(&w, "severity");
            json_int(&w, severity(d->level));
            if (d->id && *d->id) {
                json_key(&w, "code");
                json_str(&w, d->id);
            }
            json_key(&w, "source");
            json_str(&w, "cereal");
            json_key(&w, "message");
            json_str(&w, msg);
            if (d->notes.len) {
                json_key(&w, "relatedInformation");
                json_begin_array(&w);
                for (k = 0; k < d->notes.len; k++) {
                    SrcFile *nf2 = srcmgr_file_of(sm, d->notes.data[k].loc);
                    if (!nf2 || (nf2->kind != SF_USER && nf2->kind != SF_SYSTEM))
                        continue;
                    json_begin_object(&w);
                    json_key(&w, "location");
                    json_begin_object(&w);
                    json_key(&w, "uri");
                    json_str(&w, path_to_uri(&a, nf2->path));
                    json_key(&w, "range");
                    json_range(&w, sm, enc, d->notes.data[k].loc,
                               d->notes.data[k].loc);
                    json_end_object(&w);
                    json_key(&w, "message");
                    json_str(&w, d->notes.data[k].msg);
                    json_end_object(&w);
                }
                json_end_array(&w);
            }
            json_end_object(&w);
        }
        json_end_array(&w);
        json_end_object(&w);
        json_end_object(&w);
        rpc_write(sb.data, sb.len);
        sb_free(&sb);
    }
    arena_free(&a);
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
