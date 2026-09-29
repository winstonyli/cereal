/* server.c - protocol loop, documents, units and the builder.
 *
 * A *unit* is a translation unit the server keeps built: a source file,
 * or a header opened on its own until some unit is seen to include it
 * (then the header's features come from the includer, with the includer's
 * flags, as the header is really compiled).  Each unit owns its latest
 * complete snapshot.  Edits bump the unit's wanted generation and cancel
 * its running build; the builder thread builds the newest wanted state
 * from a frozen copy of the editor buffers (an Overlay). */
#include "lsp.h"

#include "../analysis/analysis.h"
#include "../mgraph.h"
#include "../par.h"

#include <string.h>

typedef struct Overlay {
    uint32_t refs;            /* atomic */
    size_t n;
    char **paths;
    char **texts;
    size_t *lens;
} Overlay;

typedef struct Unit {
    char *main;
    bool standalone_header;   /* no compile command: may be adopted */
    Snapshot *snap;           /* latest complete, owned reference */
    Interner *in;             /* shared by its builds: ids stay stable */
    uint32_t in_fresh;        /* identifiers after the interner's first build */
    long long want, built;
    uint32_t cancel;          /* atomic */
    bool queued, building;
} Unit;

typedef struct Doc {
    char *path, *uri;
    char *text;
    size_t len;
    long long version;
    Unit *unit;
} Doc;

typedef struct Server {
    Mutex m;
    Cond work, done;
    VEC(Doc *) docs;
    VEC(Unit *) units;
    VEC(Unit *) queue;
    bool stop, initialized, shutdown;
    PosEncoding enc;
    bool inactive_regions;    /* client takes clangd's inactiveRegions */
    LspConfig cfg;
    ThreadPool pool;
    long long gen;
    pthread_t builder;
} Server;

static Server S;

/* ---- overlays --------------------------------------------------------- */

static Overlay *overlay_capture(void) /* S.m held */
{
    Overlay *o = xcalloc(1, sizeof *o);
    size_t i;
    o->refs = 1;
    o->n = S.docs.len;
    o->paths = xcalloc(o->n + 1, sizeof(char *));
    o->texts = xcalloc(o->n + 1, sizeof(char *));
    o->lens = xcalloc(o->n + 1, sizeof(size_t));
    for (i = 0; i < o->n; i++) {
        Doc *d = S.docs.data[i];
        o->paths[i] = xstrdup(d->path);
        o->texts[i] = xmalloc(d->len + 1);
        memcpy(o->texts[i], d->text, d->len);
        o->texts[i][d->len] = 0;
        o->lens[i] = d->len;
    }
    return o;
}

static void overlay_release(Overlay *o)
{
    size_t i;
    if (!o || atomic_add_u32(&o->refs, (uint32_t)-1) != 1)
        return;
    for (i = 0; i < o->n; i++) {
        free(o->paths[i]);
        free(o->texts[i]);
    }
    free(o->paths);
    free(o->texts);
    free(o->lens);
    free(o);
}

static bool overlay_lookup(void *ctx, const char *path, const char **buf,
                           size_t *len)
{
    Overlay *o = ctx;
    size_t i;
    for (i = 0; i < o->n; i++)
        if (!strcmp(o->paths[i], path)) {
            *buf = o->texts[i];
            *len = o->lens[i];
            return true;
        }
    return false;
}

/* ---- snapshots ---------------------------------------------------------- */

void snapshot_release(Snapshot *s)
{
    if (!s || atomic_add_u32(&s->refs, (uint32_t)-1) != 1)
        return;
    mgraph_free(&s->graph);
    index_free(&s->ix);
    tu_free(&s->tu);
    config_options_free(s->opt);
    overlay_release(s->overlay);
    free((char *)s->main);
    free(s);
}

static Snapshot *snapshot_ref(Snapshot *s)
{
    if (s)
        atomic_add_u32(&s->refs, 1);
    return s;
}

static void start_tu(Snapshot *s, Analysis *an, Interner *in, uint32_t *cancel)
{
    tu_init_shared(&s->tu, s->opt, in);
    s->tu.sm.overlay = overlay_lookup;
    s->tu.sm.overlay_ctx = s->overlay;
    s->tu.pp.cancel = cancel;
    memset(an, 0, sizeof *an);
    analysis_attach(an, &s->tu.pp);
    index_init(&s->ix, &s->tu.pp);
}

/* Build a unit: preprocess, analyze and index, in parallel when the file
 * is large.  NULL if cancelled. */
static Snapshot *build(const char *main, Overlay *ov, Options *opt,
                       Interner *in, uint32_t *cancel)
{
    Snapshot *s = xcalloc(1, sizeof *s);
    Analysis an;
    ParOptions po;
    ParClient cs[2];
    ParResult r;
    s->refs = 1;
    s->main = xstrdup(main);
    s->opt = opt;
    s->overlay = ov;
    start_tu(s, &an, in, cancel);
    memset(&po, 0, sizeof po);
    po.pool = &S.pool;
    {
        const char *buf;
        size_t len;
        if (overlay_lookup(ov, main, &buf, &len))
            po.size_hint = len ? len : 1;
    }
    cs[0] = analysis_par_client(&an);
    cs[1] = index_par_client(&s->ix);
    r = par_run(&s->tu, main, NULL, false, &po, cs, 2);
    if (r == PAR_DONE) {
        analysis_finish(&an);
    } else if (r == PAR_FALLBACK) {
        index_free(&s->ix);
        tu_free(&s->tu);
        start_tu(s, &an, in, cancel);
        if (tu_begin(&s->tu, main)) {
            index_run(&s->ix);
            analysis_finish(&an);
        }
    }
    /* complete: the listeners' state (analysis) was on this stack */
    s->tu.pp.listeners.len = 0;
    if (atomic_load_u32(cancel)) {
        s->overlay = NULL; /* the caller still owns it on failure */
        s->opt = NULL;
        index_free(&s->ix);
        tu_free(&s->tu);
        free((char *)s->main);
        free(s);
        return NULL;
    }
    mgraph_build(&s->graph, &s->tu.pp);
    return s;
}

/* ---- units --------------------------------------------------------------- */

static bool has_command(const char *path)
{
    size_t i;
    for (i = 0; i < S.cfg.cmds.len; i++)
        if (!strcmp(S.cfg.cmds.data[i].file, path))
            return true;
    return false;
}

static bool is_header(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && (!strcmp(dot, ".h") || !strcmp(dot, ".inc") ||
                   !strcmp(dot, ".def"));
}

static bool snapshot_has(Snapshot *s, const char *path)
{
    uint32_t i, n;
    if (!s)
        return false;
    n = srcmgr_nfiles(&s->tu.sm);
    for (i = 0; i < n; i++) {
        SrcFile *f = srcmgr_file(&s->tu.sm, i);
        if (f->kind == SF_USER && !strcmp(f->path, path))
            return true;
    }
    return false;
}

static Unit *unit_named(const char *main) /* S.m held */
{
    size_t i;
    Unit *u;
    for (i = 0; i < S.units.len; i++)
        if (!strcmp(S.units.data[i]->main, main))
            return S.units.data[i];
    u = xcalloc(1, sizeof *u);
    u->main = xstrdup(main);
    u->standalone_header = !has_command(main) && is_header(main);
    vec_push(&S.units, u);
    return u;
}

/* The unit giving a document its features: its own, or for a header
 * without a compile command, a unit that includes it. */
static Unit *unit_for(const char *path) /* S.m held */
{
    size_t i;
    if (!has_command(path) && is_header(path))
        for (i = 0; i < S.units.len; i++)
            if (!S.units.data[i]->standalone_header &&
                snapshot_has(S.units.data[i]->snap, path))
                return S.units.data[i];
    return unit_named(path);
}

static void schedule(Unit *u) /* S.m held */
{
    u->want = ++S.gen;
    atomic_store_u32(&u->cancel, 1); /* a running build is stale now */
    if (!u->queued) {
        u->queued = true;
        vec_push(&S.queue, u);
    }
    cond_broadcast(&S.work);
}

/* After a snapshot of u: headers opened on their own that u includes move
 * to u. */
static void adopt_headers(Unit *u) /* S.m held */
{
    size_t i;
    if (u->standalone_header)
        return;
    for (i = 0; i < S.docs.len; i++) {
        Doc *d = S.docs.data[i];
        if (d->unit != u && d->unit->standalone_header &&
            snapshot_has(u->snap, d->path))
            d->unit = u;
    }
}

static bool doc_open_in(void *ctx, const char *path)
{
    Unit *u = ctx;
    size_t i;
    for (i = 0; i < S.docs.len; i++)
        if (S.docs.data[i]->unit == u && !strcmp(S.docs.data[i]->path, path))
            return true;
    return false;
}

static void *builder_main(void *arg)
{
    (void)arg;
    mutex_lock(&S.m);
    while (!S.stop) {
        Unit *u;
        Overlay *ov;
        Options *opt;
        Snapshot *snap, *old = NULL;
        long long want;
        if (!S.queue.len) {
            cond_wait(&S.work, &S.m);
            continue;
        }
        u = S.queue.data[0];
        memmove(S.queue.data, S.queue.data + 1,
                sizeof(Unit *) * --S.queue.len);
        u->queued = false;
        u->building = true;
        want = u->want;
        atomic_store_u32(&u->cancel, 0);
        ov = overlay_capture();
        mutex_unlock(&S.m);

        /* names typed and deleted pile up in a shared interner: start a
         * new one when it has grown well past what a build needs */
        if (u->in && interner_count(u->in) > 2 * u->in_fresh + 65536) {
            interner_release(u->in);
            u->in = NULL;
        }
        if (!u->in) {
            u->in = interner_new();
            u->in_fresh = 0;
        }
        opt = config_options_for(&S.cfg, u->main);
        snap = build(u->main, ov, opt, u->in, &u->cancel);
        if (snap && !u->in_fresh)
            u->in_fresh = interner_count(u->in);
        if (!snap) {
            overlay_release(ov);
            config_options_free(opt);
        }

        mutex_lock(&S.m);
        u->building = false;
        if (snap && want > u->built) {
            old = u->snap;
            u->snap = snap;
            u->built = want;
            adopt_headers(u);
            lsp_publish_diagnostics(snap, S.enc, doc_open_in, u);
            if (S.inactive_regions) {
                size_t i;
                for (i = 0; i < S.docs.len; i++)
                    if (S.docs.data[i]->unit == u)
                        lsp_publish_inactive(snap, S.enc,
                                             S.docs.data[i]->path);
            }
        } else if (snap) {
            snapshot_release(snap);
        }
        cond_broadcast(&S.done);
        mutex_unlock(&S.m);
        snapshot_release(old);
        mutex_lock(&S.m);
    }
    mutex_unlock(&S.m);
    return NULL;
}

/* ---- documents ------------------------------------------------------------ */

static Doc *doc_find(const char *path) /* S.m held */
{
    size_t i;
    for (i = 0; i < S.docs.len; i++)
        if (!strcmp(S.docs.data[i]->path, path))
            return S.docs.data[i];
    return NULL;
}

static void doc_free(Doc *d)
{
    free(d->path);
    free(d->uri);
    free(d->text);
    free(d);
}

/* Apply one contentChanges entry: a range edit, or the whole text. */
static void apply_change(Doc *d, const JsonValue *ch)
{
    const JsonValue *range = json_get(ch, "range");
    const char *text = json_str_of(json_get(ch, "text"), "");
    size_t tl = strlen(text);
    if (!range) {
        free(d->text);
        d->text = xstrdup(text);
        d->len = tl;
        return;
    }
    {
        const JsonValue *st = json_get(range, "start"),
                        *en = json_get(range, "end");
        size_t a = pos_to_offset(d->text, d->len,
                                 (uint32_t)json_int_of(json_get(st, "line"), 0),
                                 (uint32_t)json_int_of(json_get(st, "character"), 0),
                                 S.enc);
        size_t b = pos_to_offset(d->text, d->len,
                                 (uint32_t)json_int_of(json_get(en, "line"), 0),
                                 (uint32_t)json_int_of(json_get(en, "character"), 0),
                                 S.enc);
        char *nt;
        if (b < a)
            b = a;
        nt = xmalloc(d->len - (b - a) + tl + 1);
        memcpy(nt, d->text, a);
        memcpy(nt + a, text, tl);
        memcpy(nt + a + tl, d->text + b, d->len - b);
        d->len = d->len - (b - a) + tl;
        nt[d->len] = 0;
        free(d->text);
        d->text = nt;
    }
}

/* ---- responses ---------------------------------------------------------- */

static void send_json(StrBuf *sb)
{
    rpc_write(sb->data, sb->len);
}

static void begin_response(JsonWriter *w, StrBuf *sb, const JsonValue *id)
{
    json_init_buf(w, sb);
    json_begin_object(w);
    json_key(w, "jsonrpc");
    json_str(w, "2.0");
    json_key(w, "id");
    if (id)
        json_raw(w, id->text, id->text_len);
    else
        json_null(w);
}

static void respond_error(const JsonValue *id, int code, const char *msg)
{
    StrBuf sb = {0};
    JsonWriter w;
    begin_response(&w, &sb, id);
    json_key(&w, "error");
    json_begin_object(&w);
    json_key(&w, "code");
    json_int(&w, code);
    json_key(&w, "message");
    json_str(&w, msg);
    json_end_object(&w);
    json_end_object(&w);
    send_json(&sb);
    sb_free(&sb);
}

static void respond_null(const JsonValue *id)
{
    StrBuf sb = {0};
    JsonWriter w;
    begin_response(&w, &sb, id);
    json_key(&w, "result");
    json_null(&w);
    json_end_object(&w);
    send_json(&sb);
    sb_free(&sb);
}

static void initialize(const JsonValue *id, const JsonValue *params)
{
    StrBuf sb = {0};
    JsonWriter w;
    const JsonValue *encs =
        json_path(params, "capabilities.general.positionEncodings");
    const char *root_uri = json_str_of(json_get(params, "rootUri"), NULL);
    const char *root = NULL;
    Arena a;
    size_t i;
    arena_init(&a);
    S.enc = ENC_UTF16;
    for (i = 0; encs && encs->kind == JV_ARR && i < encs->len; i++)
        if (!strcmp(json_str_of(encs->items[i], ""), "utf-8"))
            S.enc = ENC_UTF8;
    S.inactive_regions = json_bool_of(
        json_path(params, "capabilities.textDocument.inactiveRegionsCapabilities"
                          ".inactiveRegions"), false);
    if (root_uri)
        root = uri_to_path(&a, root_uri);
    if (!root)
        root = json_str_of(json_get(params, "rootPath"), NULL);
    config_init(&S.cfg, root);
    arena_free(&a);

    begin_response(&w, &sb, id);
    json_key(&w, "result");
    json_begin_object(&w);
    json_key(&w, "capabilities");
    json_begin_object(&w);
    json_key(&w, "positionEncoding");
    json_str(&w, S.enc == ENC_UTF8 ? "utf-8" : "utf-16");
    json_key(&w, "textDocumentSync");
    json_begin_object(&w);
    json_key(&w, "openClose");
    json_bool(&w, true);
    json_key(&w, "change");
    json_int(&w, 2); /* incremental */
    json_end_object(&w);
    json_key(&w, "definitionProvider");
    json_bool(&w, true);
    json_key(&w, "referencesProvider");
    json_bool(&w, true);
    json_key(&w, "hoverProvider");
    json_bool(&w, true);
    json_key(&w, "completionProvider");
    json_begin_object(&w);
    json_key(&w, "resolveProvider");
    json_bool(&w, false);
    json_end_object(&w);
    json_key(&w, "documentSymbolProvider");
    json_bool(&w, true);
    json_key(&w, "semanticTokensProvider");
    json_begin_object(&w);
    json_key(&w, "legend");
    json_begin_object(&w);
    json_key(&w, "tokenTypes");
    json_begin_array(&w);
    json_str(&w, "macro");
    json_str(&w, "parameter");
    json_end_array(&w);
    json_key(&w, "tokenModifiers");
    json_begin_array(&w);
    json_str(&w, "declaration");
    json_str(&w, "readonly");
    json_end_array(&w);
    json_end_object(&w);
    json_key(&w, "full");
    json_bool(&w, true);
    json_end_object(&w);
    json_key(&w, "foldingRangeProvider");
    json_bool(&w, true);
    json_key(&w, "renameProvider");
    json_begin_object(&w);
    json_key(&w, "prepareProvider");
    json_bool(&w, true);
    json_end_object(&w);
    json_key(&w, "callHierarchyProvider");
    json_bool(&w, true);
    json_key(&w, "signatureHelpProvider");
    json_begin_object(&w);
    json_key(&w, "triggerCharacters");
    json_begin_array(&w);
    json_str(&w, "(");
    json_str(&w, ",");
    json_end_array(&w);
    json_end_object(&w);
    json_key(&w, "inactiveRegionsProvider"); /* clangd extension */
    json_bool(&w, true);
    json_end_object(&w);
    json_key(&w, "serverInfo");
    json_begin_object(&w);
    json_key(&w, "name");
    json_str(&w, "cereal");
    json_end_object(&w);
    json_end_object(&w);
    json_end_object(&w);
    send_json(&sb);
    sb_free(&sb);
    S.initialized = true;
}

/* ---- requests ------------------------------------------------------------ */

typedef enum {
    R_DEF, R_REFS, R_HOVER, R_COMPLETION, R_SYMBOLS, R_SEMTOK, R_FOLDING,
    R_PREP_RENAME, R_RENAME, R_PREP_CALLS, R_IN_CALLS, R_OUT_CALLS,
    R_SIGHELP, R_EXPAND
} ReqKind;

static const struct {
    const char *method;
    ReqKind kind;
} REQS[] = {
    {"textDocument/definition", R_DEF},
    {"textDocument/declaration", R_DEF},
    {"textDocument/references", R_REFS},
    {"textDocument/hover", R_HOVER},
    {"textDocument/completion", R_COMPLETION},
    {"textDocument/documentSymbol", R_SYMBOLS},
    {"textDocument/semanticTokens/full", R_SEMTOK},
    {"textDocument/foldingRange", R_FOLDING},
    {"textDocument/prepareRename", R_PREP_RENAME},
    {"textDocument/rename", R_RENAME},
    {"textDocument/prepareCallHierarchy", R_PREP_CALLS},
    {"callHierarchy/incomingCalls", R_IN_CALLS},
    {"callHierarchy/outgoingCalls", R_OUT_CALLS},
    {"textDocument/signatureHelp", R_SIGHELP},
    {"cereal/expandMacro", R_EXPAND},
    {NULL, R_DEF}};

/* The document a request is about: textDocument.uri, or item.uri for
 * call hierarchy follow-ups. */
static const char *request_uri(const JsonValue *params)
{
    const char *u = json_str_of(json_path(params, "textDocument.uri"), NULL);
    return u ? u : json_str_of(json_path(params, "item.uri"), NULL);
}

static void handle_request(const JsonValue *id, ReqKind k,
                           const JsonValue *params)
{
    Arena a;
    Req r;
    const char *uri = request_uri(params);
    char *path;
    Doc *d;
    Unit *u;
    StrBuf sb = {0};
    JsonWriter w;
    const char *err = NULL;
    arena_init(&a);
    path = uri ? uri_to_path(&a, uri) : NULL;
    memset(&r, 0, sizeof r);
    mutex_lock(&S.m);
    d = path ? doc_find(path) : NULL;
    if (!d) {
        mutex_unlock(&S.m);
        arena_free(&a);
        respond_null(id);
        return;
    }
    u = d->unit;
    while (!u->snap && !S.stop) /* first build of this unit */
        cond_wait(&S.done, &S.m);
    r.snap = snapshot_ref(u->snap);
    r.text = arena_strndup(&a, d->text, d->len);
    r.text_len = d->len;
    mutex_unlock(&S.m);
    if (!r.snap) {
        arena_free(&a);
        respond_null(id);
        return;
    }
    r.path = path;
    r.enc = S.enc;
    r.arena = &a;
    r.params = params;
    r.file = index_find_file(&r.snap->ix, path);

    begin_response(&w, &sb, id);
    json_key(&w, "result");
    if (!r.file) {
        json_null(&w);
    } else {
        switch (k) {
        case R_DEF: lsp_definition(&r, &w); break;
        case R_REFS: lsp_references(&r, &w); break;
        case R_HOVER: lsp_hover(&r, &w); break;
        case R_COMPLETION: lsp_completion(&r, &w); break;
        case R_SYMBOLS: lsp_document_symbols(&r, &w); break;
        case R_SEMTOK: lsp_semantic_tokens(&r, &w); break;
        case R_FOLDING: lsp_folding(&r, &w); break;
        case R_PREP_RENAME: lsp_prepare_rename(&r, &w); break;
        case R_RENAME:
            if (!lsp_rename(&r, &w, &err)) {
                sb.len = 0;
                snapshot_release(r.snap);
                respond_error(id, -32803 /* RequestFailed */, err);
                arena_free(&a);
                sb_free(&sb);
                return;
            }
            break;
        case R_PREP_CALLS: lsp_prepare_call_hierarchy(&r, &w); break;
        case R_IN_CALLS: lsp_calls(&r, &w, true); break;
        case R_OUT_CALLS: lsp_calls(&r, &w, false); break;
        case R_SIGHELP: lsp_signature_help(&r, &w); break;
        case R_EXPAND: lsp_expand_macro(&r, &w); break;
        }
    }
    json_end_object(&w);
    send_json(&sb);
    sb_free(&sb);
    snapshot_release(r.snap);
    arena_free(&a);
}

static void did_open(const JsonValue *params)
{
    const JsonValue *td = json_get(params, "textDocument");
    const char *uri = json_str_of(json_get(td, "uri"), NULL);
    const char *text = json_str_of(json_get(td, "text"), "");
    Arena a;
    char *path;
    Doc *d;
    if (!uri)
        return;
    arena_init(&a);
    path = uri_to_path(&a, uri);
    if (path) {
        mutex_lock(&S.m);
        if (!(d = doc_find(path))) {
            d = xcalloc(1, sizeof *d);
            d->path = xstrdup(path);
            d->uri = xstrdup(uri);
            vec_push(&S.docs, d);
        }
        free(d->text);
        d->text = xstrdup(text);
        d->len = strlen(text);
        d->version = json_int_of(json_get(td, "version"), 0);
        d->unit = unit_for(path);
        schedule(d->unit);
        mutex_unlock(&S.m);
    }
    arena_free(&a);
}

static void did_change(const JsonValue *params)
{
    const char *uri = json_str_of(json_path(params, "textDocument.uri"), NULL);
    const JsonValue *changes = json_get(params, "contentChanges");
    Arena a;
    char *path;
    Doc *d;
    size_t i;
    if (!uri)
        return;
    arena_init(&a);
    path = uri_to_path(&a, uri);
    mutex_lock(&S.m);
    if (path && (d = doc_find(path)) != NULL) {
        for (i = 0; changes && changes->kind == JV_ARR && i < changes->len; i++)
            apply_change(d, changes->items[i]);
        d->version = json_int_of(json_path(params, "textDocument.version"),
                                 d->version + 1);
        schedule(d->unit);
    }
    mutex_unlock(&S.m);
    arena_free(&a);
}

static void did_close(const JsonValue *params)
{
    const char *uri = json_str_of(json_path(params, "textDocument.uri"), NULL);
    Arena a;
    char *path;
    size_t i;
    if (!uri)
        return;
    arena_init(&a);
    path = uri_to_path(&a, uri);
    mutex_lock(&S.m);
    for (i = 0; path && i < S.docs.len; i++)
        if (!strcmp(S.docs.data[i]->path, path)) {
            Doc *d = S.docs.data[i];
            Unit *u = d->unit;
            S.docs.data[i] = S.docs.data[--S.docs.len];
            doc_free(d);
            schedule(u); /* back to the file on disk */
            break;
        }
    mutex_unlock(&S.m);
    if (uri) { /* clear its diagnostics */
        StrBuf sb = {0};
        JsonWriter w;
        json_init_buf(&w, &sb);
        json_begin_object(&w);
        json_key(&w, "jsonrpc");
        json_str(&w, "2.0");
        json_key(&w, "method");
        json_str(&w, "textDocument/publishDiagnostics");
        json_key(&w, "params");
        json_begin_object(&w);
        json_key(&w, "uri");
        json_str(&w, uri);
        json_key(&w, "diagnostics");
        json_begin_array(&w);
        json_end_array(&w);
        json_end_object(&w);
        json_end_object(&w);
        send_json(&sb);
        sb_free(&sb);
    }
    arena_free(&a);
}

/* ---- main loop ------------------------------------------------------------ */

int lsp_main(FILE *in, FILE *out)
{
    char *body;
    size_t len;
    int rc = 1;
    memset(&S, 0, sizeof S);
    mutex_init(&S.m);
    cond_init(&S.work);
    cond_init(&S.done);
    pool_init(&S.pool, 0);
    rpc_set_output(out);
    if (pthread_create(&S.builder, NULL, builder_main, NULL) != 0)
        fatal("cannot start the builder thread");
    while ((body = rpc_read(in, &len)) != NULL) {
        Arena a;
        JsonValue *msg, *id, *params;
        const char *method;
        arena_init(&a);
        msg = json_parse(&a, body, len); /* values point into body */
        if (!msg || msg->kind != JV_OBJ) {
            respond_error(NULL, -32700, "parse error");
            free(body);
            arena_free(&a);
            continue;
        }
        method = json_str_of(json_get(msg, "method"), NULL);
        id = json_get(msg, "id");
        params = json_get(msg, "params");
        if (!method) { /* a response to something we never send */
            free(body);
            arena_free(&a);
            continue;
        }
        if (!strcmp(method, "exit")) {
            rc = S.shutdown ? 0 : 1;
            free(body);
            arena_free(&a);
            break;
        }
        if (!strcmp(method, "initialize")) {
            initialize(id, params);
        } else if (!S.initialized) {
            if (id)
                respond_error(id, -32002, "server not initialized");
        } else if (!strcmp(method, "shutdown")) {
            S.shutdown = true;
            respond_null(id);
        } else if (!strcmp(method, "textDocument/didOpen")) {
            did_open(params);
        } else if (!strcmp(method, "textDocument/didChange")) {
            did_change(params);
        } else if (!strcmp(method, "textDocument/didClose")) {
            did_close(params);
        } else if (!strcmp(method, "cereal/waitIdle") && id) {
            /* answers once no build is queued or running: everything
             * published so far is current (a barrier for tests) */
            size_t i;
            bool busy;
            mutex_lock(&S.m);
            do {
                busy = S.queue.len > 0;
                for (i = 0; i < S.units.len && !busy; i++)
                    busy = S.units.data[i]->building;
                if (busy)
                    cond_wait(&S.done, &S.m);
            } while (busy && !S.stop);
            mutex_unlock(&S.m);
            respond_null(id);
        } else if (id) {
            int i;
            for (i = 0; REQS[i].method; i++)
                if (!strcmp(method, REQS[i].method))
                    break;
            if (REQS[i].method)
                handle_request(id, REQS[i].kind, params);
            else
                respond_error(id, -32601, "method not found");
        }
        /* other notifications ($/cancelRequest, didSave, initialized, ...)
         * need nothing: requests are answered synchronously */
        free(body);
        arena_free(&a);
    }
    mutex_lock(&S.m);
    S.stop = true;
    cond_broadcast(&S.work);
    cond_broadcast(&S.done);
    mutex_unlock(&S.m);
    pthread_join(S.builder, NULL);
    {
        size_t i;
        for (i = 0; i < S.units.len; i++) {
            snapshot_release(S.units.data[i]->snap);
            interner_release(S.units.data[i]->in);
            free(S.units.data[i]->main);
            free(S.units.data[i]);
        }
        for (i = 0; i < S.docs.len; i++)
            doc_free(S.docs.data[i]);
        vec_free(&S.units);
        vec_free(&S.docs);
        vec_free(&S.queue);
    }
    if (S.initialized)
        config_free(&S.cfg);
    pool_free(&S.pool);
    return rc;
}
