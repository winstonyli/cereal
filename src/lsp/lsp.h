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

typedef struct CompileCmd {
    const char *file;        /* absolute, normalized */
    const char *dir;
    const char **argv;
    int argc;
} CompileCmd;

typedef struct LspConfig {
    Arena arena;
    const char *root;        /* workspace root (absolute) or NULL */
    VEC(CompileCmd) cmds;
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
/* Split a command line like a POSIX shell (quotes, backslashes). */
int shell_split(Arena *a, const char *s, const char ***argv);

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

/* A compiler diagnostic as published, kept to be carried over. */
typedef struct CDiag {
    char *path;              /* the file it is published for */
    uint32_t last_line;      /* its last line there (range and notes) */
    bool other_files;        /* a note points into another file */
    char *key;               /* range and message: duplicates are dropped */
    char *json;              /* the publishDiagnostics array element */
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
    /* The C symbol index of the snapshot's check (phase 2), published under
     * the server lock; NULL until then, or with no check (CHECK_NONE). */
    struct CIndex *cidx;
    int check_state;         /* CHECK_* */
    /* The compiler diagnostics published with the snapshot: its check's
     * once that ends, before then those carried over from the previous
     * snapshot (lines before the first edited one).  Builder only. */
    VEC(CDiag) cdiags;
    char *notice;            /* why the unit is not checked (Information) */
} Snapshot;

enum { CHECK_NONE, CHECK_PENDING, CHECK_DONE };

void snapshot_release(Snapshot *s);

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
    const struct CIndex *cidx; /* snap's, taken under the lock; NULL: none */
} Req;

/* Each writes the result value (not the envelope). */
void lsp_definition(Req *r, JsonWriter *w);
void lsp_declaration(Req *r, JsonWriter *w);
void lsp_references(Req *r, JsonWriter *w);
void lsp_hover(Req *r, JsonWriter *w);
void lsp_completion(Req *r, JsonWriter *w);
void lsp_document_symbols(Req *r, JsonWriter *w);
/* full (previous_id NULL) or full/delta */
void lsp_semantic_tokens(Req *r, JsonWriter *w, const char *previous_id);
void lsp_semantic_tokens_range(Req *r, JsonWriter *w);
/* Drop the semantic tokens kept for deltas (path NULL: all). */
void lsp_forget_tokens(const char *path);
void lsp_folding(Req *r, JsonWriter *w);
void lsp_prepare_rename(Req *r, JsonWriter *w);
/* false: the rename is refused; *err set */
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
