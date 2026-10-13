/* cidxrec.h - the project index's records and manifests
 * (docs/B5_DESIGN.md section 5): what one translation unit exports, in a
 * binary form that stays valid across runs and units.
 *
 * A translation unit yields one manifest (the files it read, with stat data,
 * content hashes and record digests) and one record per user file with
 * exports (declarations, references, calls, macro definitions).  Records are
 * self-contained and sorted, so two units that see a header alike write
 * byte-identical records; a record is named by the digest of its bytes.
 *
 * Entries are native-endian structs with natural alignment; the header's byte
 * order mark makes another host's file a miss.  The reader validates a whole
 * file once (sizes, checksum, every string offset and key index), so views
 * can be used without further checks. */
#ifndef CEREAL_CIDXREC_H
#define CEREAL_CIDXREC_H

#include "driver.h"

#define CREC_FORMAT 1u           /* bumped on any change of a section's layout */
enum { CREC_MANIFEST = 1, CREC_RECORD = 2 };
enum { CTU_OK = 1, CTU_PARTIAL, CTU_OVER_LIMIT, CTU_FAILED };
/* CRecFil.flags */
enum { CFIL_SYSTEM = 1, CFIL_USER = 2, CFIL_CONFIG = 4 };

typedef struct CRecHeader {      /* 32 bytes */
    char magic[8];               /* "CRLIDX\0\0" */
    uint32_t bom;                /* 0x01020304 as the host writes it */
    uint16_t version, kind;
    uint64_t build_id;
    uint32_t nsections;
    uint32_t payload_hash;       /* low 32 bits of hash64 of all after the header */
} CRecHeader;

typedef struct CRecSecDir {      /* after the header, one per section */
    uint32_t tag, offset, count, bytes;
} CRecSecDir;

/* Entries.  Every u32 named path, name, container ... is an offset into the
 * file's string blob (0 is ""). */
typedef struct CRecFile {        /* FILE: the file the record describes */
    uint32_t path, size;
    uint64_t content_hash;       /* cindex_hash of its text */
} CRecFile;
typedef struct CRecKey {         /* KEYS: identity keys (cindex_key) */
    uint32_t name;
    uint8_t kind, linkage;
    uint16_t kflags;             /* reserved: 0 */
    uint32_t path, off;
} CRecKey;
typedef struct CRecDecl {        /* DECL: declarations and definitions here */
    uint32_t key, off;
    uint16_t len;
    uint8_t role;                /* CIX_DECL or CIX_DEF */
    uint8_t pad;
    uint32_t container;          /* the record's or enum's name, for a member */
    uint32_t ext_begin, ext_end; /* the external declaration */
    uint32_t flags;              /* CIDF_* */
} CRecDecl;
typedef struct CRecRef {         /* REFS */
    uint32_t key, off;
    uint8_t len;                 /* 255: the key's name length */
    uint8_t flags;               /* CIX_CALL, ARG, AT_EXPANSION, MACRO_BODY */
    uint16_t pad;
} CRecRef;
typedef struct CRecCall {        /* CALL: sites in this file */
    uint32_t caller, callee, off;
    uint8_t len, flags;          /* CIX_AT_EXPANSION: an xcall */
    uint16_t pad;
} CRecCall;
typedef struct CRecMacro {       /* MACR: user macro definitions */
    uint32_t name, off;
    uint16_t len;
    uint8_t funclike, pad;
} CRecMacro;
typedef struct CRecTu {          /* TU: the manifest's one entry */
    uint64_t flag_hash, started_ns, written_ns;
    uint32_t main_path, status, errors, bytes_read, index_ms, pad;
} CRecTu;
typedef struct CRecFil {         /* FILS: every file the unit read */
    uint32_t path, size;
    uint64_t mtime_ns, content_hash;
    uint64_t record_digest;      /* 0: no exports */
    uint32_t flags, pad;
} CRecFil;
typedef struct CRecMiss {        /* MISS: includes that were not found */
    uint32_t spelled, from_path, angled;
} CRecMiss;

/* A validated file.  Sections the file lacks are empty. */
typedef struct CRecView {
    int kind;
    uint64_t build_id;
    const char *strs;            /* the blob; ends in a NUL */
    uint32_t nstrs;
    const CRecFile *file;        /* record: one */
    const CRecKey *keys;
    const CRecDecl *decls;
    const CRecRef *refs;
    const CRecCall *calls;
    const CRecMacro *macros;
    const CRecTu *tu;            /* manifest: one */
    const CRecFil *fils;
    const CRecMiss *miss;
    uint32_t nkeys, ndecls, nrefs, ncalls, nmacros, nfils, nmiss;
} CRecView;

/* The bytes of one record or manifest and the digest that names them. */
typedef struct CRecBlob {
    char *data;
    size_t len;
    uint64_t digest;             /* of the bytes after the header, seeded with
                                    build_id ^ CREC_FORMAT */
    char *path;                  /* record: the file it describes */
} CRecBlob;

typedef struct CRecSet {
    CRecBlob manifest;
    CRecBlob *recs;
    size_t nrecs;
    uint64_t tukey;              /* names the manifest */
    char *main;                  /* the unit's main file */
    int status;                  /* CTU_* */
} CRecSet;

/* hash64 of the running executable (docs/B5_DESIGN.md 5.4; Q3); 0 if it
 * cannot be read.  A few ms: call once. */
uint64_t crec_build_id(void);

/* Validates p[0..n) as a file of this kind written by this build; NULL if
 * good, else why not.  *v points into p. */
const char *crec_parse(const void *p, size_t n, int kind, uint64_t build_id,
                       CRecView *v);
/* The kind a header claims, 0 if it is no such file. */
int crec_kind(const void *p, size_t n);

/* Checks the unit `path` with o (as the language server does: a missing
 * include is an error, the check goes on), builds its symbol index and turns
 * it into a manifest and records.  cfg: the files read to build o (.cereal
 * layers, response files).  False: the file could not be opened.  Free with
 * crec_free. */
bool crec_index(Options *o, const char *path, const StrVec *cfg,
                uint64_t build_id, CRecSet *out);
void crec_free(CRecSet *s);

/* Writes the set under dir (a project directory, 5.1): records not there
 * yet, then the manifest; atomic per file, directories 0700.  Err gets the
 * reason on false. */
bool crec_write(const char *dir, const CRecSet *s, char *err, size_t errn);

/* The text form of 14.2: the manifest and each record, paths relative to
 * root (NULL: as they are), offsets as line:col and byte (the files are
 * read again for the lines).  Timings and the build-dependent digests are
 * left out, so the text is the same on every host and build. */
void crec_dump(const CRecSet *s, const char *root, FILE *out);

#endif
