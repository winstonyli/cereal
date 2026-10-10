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
} LspConfig;

void config_init(LspConfig *c, const char *root);
void config_free(LspConfig *c);
/* Options for a file: its compile command (or a nearby file's, for
 * headers and files without one), then any .cereal files from the root
 * down to the file's directory.  Heap-allocated; free with
 * config_options_free. */
Options *config_options_for(LspConfig *c, const char *path);
void config_options_free(Options *o);

/* ---- positions (pos.c) ----------------------------------------------- */

typedef enum { ENC_UTF16, ENC_UTF8 } PosEncoding;

char *uri_to_path(Arena *a, const char *uri);    /* NULL: not file:// */
char *path_to_uri(Arena *a, const char *path);
/* Byte offset of an LSP position in text (clamped to the line). */
size_t pos_to_offset(const char *text, size_t len, uint32_t line,
                     uint32_t character, PosEncoding enc);
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

typedef struct CDiag {
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
} Snapshot;

enum { CHECK_NONE, CHECK_PENDING, CHECK_DONE };

void snapshot_release(Snapshot *s);

struct CRename;
/* c_rename (c/frontend.h) for a name of snap's unit, with the buffers snap
 * was built from, on the builder thread (the checker keeps static state:
 * two checks must not overlap); NULL or why it is refused (malloc'd). */
char *lsp_check_rename(Snapshot *snap, struct CRename *q);

int lsp_main(FILE *in, FILE *out);

/* ---- requests (features.c) -------------------------------------------- */

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
    bool c_carried;          /* cidx is carried over an edit (may be stale) */
    bool c_fresh;            /* snap has the newest edit and its own index */
} Req;

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
void json_location(JsonWriter *w, Req *r, SrcLoc b, SrcLoc e);
void json_range(JsonWriter *w, SrcMgr *sm, PosEncoding enc, SrcLoc b,
                SrcLoc e);

#endif
