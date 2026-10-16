/* lsp.h - the language server (`cereal lsp`), stage 1 (docs/LSP.md).
 *
 * One protocol thread reads JSON-RPC from stdin and answers requests from
 * the latest complete snapshot of each translation unit; a builder thread
 * re-preprocesses and re-indexes translation units after edits, cancelling
 * a build that a newer edit made stale.  Snapshots are immutable and
 * reference counted, so requests never wait for a build. */
#ifndef CEREAL_LSP_H
#define CEREAL_LSP_H

#include "../c/csymidx.h"
#include "../compdb.h"
#include "../driver.h"
#include "../fixmode.h"
#include "../index.h"
#include "../json.h"
#include "../thread.h"

/* ---- transport (rpc.c) ---------------------------------------------- */

/* Read one framed message body (malloc'd); NULL at end of input. */
char *rpc_read(FILE *in, size_t *len);
/* Write one framed message; thread-safe. */
void rpc_write(const char *body, size_t len);
void rpc_set_output(FILE *out);

/* ---- configuration (config.c) ---------------------------------------- */

typedef struct LspConfig {
    Arena arena;
    const char *root;        /* workspace root (absolute) or NULL */
    CompileDb cmds;
    const char *db_path;     /* compile_commands.json in use, if any */
    unsigned refs;           /* the server's current slot and what reads it (server.c) */
} LspConfig;

/* db: the compile database to use (NULL: compile_commands.json or
 * build/compile_commands.json under root). */
void config_init(LspConfig *c, const char *root, const char *db);
void config_free(LspConfig *c);
/* The same commands (the same files, directories and arguments) in the same order. */
bool config_same(const LspConfig *x, const LspConfig *y);
/* Options for a file: its compile command (or a nearby file's, for
 * headers and files without one), then any .cereal files from the root
 * down to the file's directory.  Heap-allocated; free with
 * config_options_free. */
Options *config_options_for(LspConfig *c, const char *path);
/* The same into the caller's o (options_init'ed), with the .cereal arguments allocated in `a`
 * (which must outlive o; o->rsp will do) and nothing in the config's arena:
 * c is only read, so any thread may call it.  files (or NULL): the .cereal
 * layers and response files read are appended (paths in `a`). */
void config_options_in(LspConfig *c, const char *path, Arena *a, Options *o,
                       StrVec *files);
/* The same for compile database entry `entry` itself (not the first entry of
 * its file): a file compiled under two flag sets has two. */
void config_options_entry(LspConfig *c, size_t entry, Arena *a, Options *o,
                          StrVec *files);
void config_options_free(Options *o);

/* ---- positions (pos.c) ----------------------------------------------- */

typedef enum { ENC_UTF16, ENC_UTF8 } PosEncoding;

char *uri_to_path(Arena *a, const char *uri);    /* NULL: not file:// */
char *path_to_uri(Arena *a, const char *path);
/* Byte offset of an LSP position in text (clamped to the line). */
size_t pos_to_offset(const char *text, size_t len, uint32_t line,
                     uint32_t character, PosEncoding enc);
/* The start offsets of the lines of a text (built once, then any number of
 * offsets are placed by a binary search; free with vec_free). */
typedef VEC(size_t) LineTable;
void line_table_build(LineTable *t, const char *text, size_t len);
/* LSP position of a byte offset in the text the table was built from (clamped). */
void line_table_pos(const LineTable *t, const char *text, size_t len, size_t off,
                    PosEncoding enc, uint32_t *line, uint32_t *character);
/* LSP position of a location. */
void loc_to_pos(SrcMgr *sm, SrcLoc loc, PosEncoding enc, uint32_t *line,
                uint32_t *character);

/* ---- snapshots and the server (server.c) ------------------------------ */

/* A compiler diagnostic as published, kept to be carried over the next edit
 * (B3_DESIGN.md 12.8): offsets in the text of the files, so that the same
 * text edit that moves the index moves it. */
typedef struct CNote {
    char *path;              /* the file the note points into */
    uint32_t off;
    char *msg;
} CNote;

/* One edit of a diagnostic's fix group (C_DESIGN.md 7.2), offsets in the
 * text of path. */
typedef struct CFix {
    char *path;
    uint32_t off_b, off_e;
    char *text;
    uint8_t app;             /* FixApplic */
} CFix;

typedef struct CDiag {
    CFix *fixes;             /* its fix-its as one atomic group; NULL: none */
    size_t nfixes;
    char *path;              /* the file it is published for */
    uint32_t off_b, off_e;   /* its range there */
    int severity;            /* LSP */
    char *code;              /* NULL: none */
    char *msg;
    CNote *notes;
    size_t nnotes;
    bool carry;              /* the checked text of every file it names is
                              * the snapshot's: the offsets are valid */
} CDiag;

typedef struct FixAllEntry {
    char *path;              /* the document */
    FixTextEdits edits;
} FixAllEntry;

typedef struct Snapshot {
    uint32_t refs;           /* atomic */
    const char *main;        /* translation unit's main file */
    long long gen;           /* build generation */
    TU tu;
    Index ix;
    Options *opt;
    struct Overlay *overlay; /* the editor buffers it was built from */
    MacroGraph graph;        /* built with the snapshot */
    /* The C symbol index (phase 2), set under the server lock.  At install
     * it is the previous snapshot's index carried over the edit (B3_DESIGN.md
     * 12; cidx_carried), replaced at publish by the snapshot's own check's;
     * NULL with no check (CHECK_NONE), or when a check failed.  Requests
     * hold a reference (cindex_ref) while they answer from it. */
    struct CIndex *cidx;
    bool cidx_carried;
    int check_state;         /* CHECK_* */
    /* The compiler diagnostics published with the snapshot: its check's
     * once that ends, before then those carried over the edit from the
     * previous snapshot (cdiags_carry).  Builder only. */
    VEC(CDiag) cdiags;
    char *notice;            /* why the unit is not checked (Information) */
    /* fix-all edits of this snapshot's text, per document (lsp_fix_all);
     * S.m.  Entries live as long as the snapshot. */
    VEC(FixAllEntry *) fixall;
} Snapshot;

enum { CHECK_NONE, CHECK_PENDING, CHECK_DONE };

void snapshot_release(Snapshot *s);

struct CRename;
/* c_rename (c/frontend.h) for a name of snap's unit, with the buffers snap
 * was built from.  Call on the builder thread (checks stay ordered beside
 * the edits), i.e. from a deferred request (Req on_builder); NULL or why
 * it is refused (malloc'd). */
char *lsp_check_rename(Snapshot *snap, struct CRename *q);

/* ---- rename across units (B5d; docs/B5_DESIGN.md 11.5) ---- */

/* The open documents, copied (server.c): the checks of other units run on
 * other threads and read these instead of the live ones. */
typedef struct DocSet DocSet;
DocSet *lsp_docs_copy(void);
void lsp_docs_free(DocSet *d);
/* A CRename overlay over d. */
bool lsp_docs_lookup(void *ctx, const char *path, const char **buf, size_t *len);
/* The open document's version (-1: the client gave none) and the hash of its
 * text now; false: it is not open. */
bool lsp_doc_state(const char *path, long long *version, uint64_t *hash);
/* tmpl's edits checked in each of the units mains[i] (compile database
 * entries[i]) on the index pool, the scheduler's jobs paused; progress goes
 * to the client's token (NULL: none).  NULL: every unit accepts, else why not
 * (malloc'd). */
char *lsp_rename_gate(const struct CRename *tmpl, char *const *mains,
                      const size_t *entries, size_t n, DocSet *docs,
                      const JsonValue *token);

int lsp_main(FILE *in, FILE *out);

/* For the project index (project.c); server.c. */
size_t lsp_check_limit(void);        /* the in-process check byte limit */
/* A configuration is reference counted: the server's current slot holds one, and
 * so does whatever reads it beyond a call (an index job).  Replaced, it is freed
 * when the last reference goes.  Options built from it own their strings. */
void lsp_config_swap(LspConfig *n);  /* n (heap) becomes current; the old one is
                                      * released */
void lsp_config_ref(LspConfig *c);   /* c is current, or already referenced */
void lsp_config_unref(LspConfig *c);
bool lsp_builder_idle(void);         /* no build queued or running */

/* `cereal index-summary ARGS` (indexsum.c): argv without the mode word. */
int indexsum_main(int argc, char **argv);

/* ---- requests (features.c) -------------------------------------------- */

struct ProjIndex;
/* The text of an open document (malloc'd copy), NULL if it is not open. */
typedef char *(*WsDocText)(const char *path, size_t *len);

typedef struct Req {
    Snapshot *snap;          /* covering the document */
    SrcFile *file;           /* the document in the snapshot */
    const char *path;
    PosEncoding enc;
    Arena *arena;            /* request scratch */
    const JsonValue *params;
    const char *text;        /* current editor text (may be newer) */
    size_t text_len;
    const SrcFile *uri_file;  /* the last file json_location named */
    const char *uri;
    const struct CIndex *cidx; /* snap's, referenced under the lock; NULL: none */
    struct ProjIndex *pi;    /* the project's index (NULL: indexing is off) ... */
    WsDocText doc_text;      /* ... and the text of an open document, to place its entries */
    bool c_carried;          /* cidx is carried over an edit (may be stale) */
    bool c_fresh;            /* snap has the newest edit and its own index */
    bool stale;              /* the document has an edit snap lacks */
    bool doc_changes;        /* the client takes documentChanges */
    bool on_builder;         /* running on the builder thread or a worker (a
                                deferred request): lsp_fix_all and lsp_rename
                                compute at once */
    bool need_builder;       /* out: lsp_fix_all or lsp_rename needs the
                                builder (nothing is written); run the request
                                there instead */
    bool need_worker;        /* out: lsp_rename across units: run the request
                                on a thread of its own, not the builder */
    bool modified;           /* out: lsp_rename refused for ContentModified */
    /* codeAction: snap's check diagnostics, copied under the lock */
    VEC(CDiag) cdiags;
} Req;

/* The fix-all edits of the document of r (C_DESIGN.md 7.3), computed on the
 * builder thread and kept with the snapshot; NULL: none could be computed,
 * or the document changed meanwhile, or (r->need_builder is then set) they
 * are not kept yet and r is not running on the builder: the protocol thread
 * does not wait for it, the request is deferred (handle_request) and run
 * again there.  Defined in server.c; the answer is valid as long as r->snap
 * is. */
const FixTextEdits *lsp_fix_all(Req *r);
/* r->cdiags: snap's, copied (caller holds the lock); free with
 * lsp_cdiags_release. */
void lsp_cdiags_copy(const Snapshot *s, Req *r);
void lsp_cdiags_release(Req *r);
void lsp_code_action(Req *r, JsonWriter *w);

/* The semantic token legend, NULL-terminated (features.c). */
extern const char *const lsp_token_types[], *const lsp_token_modifiers[];

/* Each writes the result value (not the envelope). */
void lsp_definition(Req *r, JsonWriter *w);
void lsp_declaration(Req *r, JsonWriter *w);
void lsp_type_definition(Req *r, JsonWriter *w);
void lsp_references(Req *r, JsonWriter *w);
void lsp_document_highlight(Req *r, JsonWriter *w);
void lsp_hover(Req *r, JsonWriter *w);
void lsp_completion(Req *r, JsonWriter *w);
void lsp_document_symbols(Req *r, JsonWriter *w);
/* workspace/symbol over the units that have an open document: each one's
 * snapshot and C index (NULL: none yet), referenced by the caller; docs: the
 * paths of the open documents (B4_DESIGN.md 7, clangd's two matchers). */
typedef struct WsUnit {
    Snapshot *snap;
    const struct CIndex *cx;
} WsUnit;
/* pi (or NULL): the project's names are searched too, and placed in the text of
 * the documents, open ones through doc_text. */
void lsp_workspace_symbols(const WsUnit *us, size_t n, const char *const *docs,
                           size_t ndocs, struct ProjIndex *pi, WsDocText doc_text,
                           const char *query, PosEncoding enc, Arena *a, JsonWriter *w);
/* full (previous_id NULL) or full/delta */
void lsp_semantic_tokens(Req *r, JsonWriter *w, const char *previous_id);
void lsp_semantic_tokens_range(Req *r, JsonWriter *w);
/* Drop the semantic tokens kept for deltas (path NULL: all). */
void lsp_forget_tokens(const char *path);
void lsp_folding(Req *r, JsonWriter *w);
/* false: the rename is refused; *err set */
bool lsp_prepare_rename(Req *r, JsonWriter *w, const char **err);
bool lsp_rename(Req *r, JsonWriter *w, const char **err);
void lsp_prepare_call_hierarchy(Req *r, JsonWriter *w);
void lsp_calls(Req *r, JsonWriter *w, bool incoming);
void lsp_signature_help(Req *r, JsonWriter *w);
void lsp_inlay_hints(Req *r, JsonWriter *w);
void lsp_expand_macro(Req *r, JsonWriter *w);

/* Diagnostics for the files of a snapshot (publishDiagnostics bodies):
 * the macro phase's, then the compiler's (chk's, recorded in s->cdiags;
 * else s->cdiags as carried over), then s->notice on the main file. */
void lsp_publish_diagnostics(Snapshot *s, TU *chk, PosEncoding enc,
                             bool (*wanted)(void *ctx, const char *path),
                             void *ctx);
void cdiags_free(Snapshot *s);
/* to->cdiags: from's moved through the text edit from from's texts to to's;
 * one that overlaps the edit, or names a file that is gone, is dropped. */
void cdiags_carry(Snapshot *to, Snapshot *from);
/* The editor's version of a file of the overlay; false: none was sent. */
bool lsp_overlay_version(const struct Overlay *o, const char *path,
                         long long *version);
void lsp_publish_inactive(Snapshot *s, PosEncoding enc, const char *path);

/* Helpers shared by server.c and features.c. */
SrcLoc req_loc(Req *r, const JsonValue *pos);
/* ... in another file of the snapshot (a call hierarchy item's). */
SrcLoc req_loc_in(Req *r, SrcFile *file, const JsonValue *pos);
void json_location(JsonWriter *w, Req *r, SrcLoc b, SrcLoc e);
void json_range(JsonWriter *w, SrcMgr *sm, PosEncoding enc, SrcLoc b,
                SrcLoc e);

#endif
