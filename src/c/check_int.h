/* check_int.h - the checker's internals, shared by check.c (declarations)
 * and cexpr.c (expressions and constant evaluation). */
#ifndef CEREAL_CHECK_INT_H
#define CEREAL_CHECK_INT_H

#include "c/check.h"
#include "c/ckw.h"
#include "c/lit.h"
#include "c/type.h"

#include <stdarg.h>

/* ---- symbols (ordinary identifiers) ---------------------------------- */

typedef enum { CS_OBJ, CS_FUNC, CS_TYPEDEF, CS_ENUMCONST } CSymKind;

typedef enum {
    SC_NONE, SC_TYPEDEF, SC_EXTERN, SC_STATIC, SC_AUTO, SC_REGISTER
} StorageClass;

typedef enum { LK_NONE, LK_INTERNAL, LK_EXTERNAL } Linkage;

enum {
    CSF_DEFINED = 1,         /* initialized object, function with a body */
    CSF_TENTATIVE = 2,       /* file-scope object without initializer */
    CSF_PARAM = 4,
    CSF_INLINE = 8,
    CSF_THREAD = 16,
    CSF_IMPLICIT = 32,       /* implicitly declared function */
    CSF_USED = 64,
    CSF_NORETURN = 128,
    CSF_BLOCK_EXTERN = 256,  /* first declared at block scope */
    CSF_KR_PENDING = 512,    /* K&R parameter not declared yet */
    CSF_PROTO_DEF = 1024,    /* a function defined with a prototype */
    CSF_KR_DEF = 2048,       /* a function defined K&R style */
    CSF_REGISTER_NAMED = 4096, /* file-scope register with an asm label */
    CSF_ERROR = 8192,        /* its declaration had errors */
    CSF_AUTO_TYPE = 65536    /* __auto_type: type comes from the initializer */
};

typedef struct CSym {
    uint32_t name;           /* ident */
    uint8_t kind;            /* CSymKind */
    uint8_t sc;              /* StorageClass as written */
    uint8_t linkage;
    uint32_t flags;
    uint16_t align;          /* _Alignas / aligned (bytes), 0: none */
    TypeId ty;
    SrcLoc loc;
    SrcLoc def_loc;          /* the definition, if any */
    uint64_t val;            /* enumeration constants: the value */
    TypeId vty;              /* enumeration constants: the value's type */
} CSym;

/* A symbol reference: an index into the persistent symbols (file scope,
 * external declarations) or, with SYM_LOCAL set, into the unit's. */
#define SYM_LOCAL 0x80000000u
#define SYM_NONE 0xFFFFFFFFu

/* One binding, in the log both namespaces share. */
enum { NS_ORD, NS_TAG };

typedef struct Bind {
    uint32_t ident;
    uint32_t prev;           /* the binding it shadows + 1, 0: none */
    uint32_t ref;            /* NS_ORD: symbol ref; NS_TAG: TypeId */
    uint8_t ns;
} Bind;

typedef VEC(Bind) BindVec;

#define NO_NODE 0xFFFFFFFFu

typedef enum { SCK_FILE, SCK_BLOCK, SCK_PROTO, SCK_FUNC } ScopeKind;

typedef struct ScopeMark {
    uint32_t log;            /* bindings before this scope */
    uint8_t kind;
} ScopeMark;

/* ---- declaration specifiers ------------------------------------------ */

/* Type-specifier words, as gcc counts them (cts_*). */
typedef enum {
    TW_NONE, TW_VOID, TW_BOOL, TW_CHAR, TW_INT, TW_FLOAT, TW_DOUBLE,
    TW_INT128, TW_FLOATN, TW_DECIMAL, TW_AUTO_TYPE
} TypeWord;

/* How the type was given: gcc's c_typespec_kind. */
typedef enum {
    TSK_NONE, TSK_RESERVED, TSK_TAGREF, TSK_TAGFIRSTREF, TSK_TAGDEF,
    TSK_TYPEDEF, TSK_TYPEOF
} TypeSpecKind;

/* Attributes that change types and layout. */
typedef struct Attrs {
    uint32_t aligned;        /* bytes, 0: none */
    bool packed;
    bool transparent_union;
    bool has_mode;
    uint8_t mode_bytes;      /* mode(QI..TI): integer size; 0: word etc. */
    uint8_t mode_float;      /* mode(SF/DF/XF/TF): 1 */
    uint64_t vector_size;    /* bytes, 0: none */
    bool deprecated, unused, noreturn, weak, alias, section, cleanup;
} Attrs;

/* The declaration specifiers of one declaration (gcc's c_declspecs). */
typedef struct Spec {
    TypeId ty;               /* the base type, unqualified by the specifiers */
    uint32_t node;           /* the SPECS node */
    uint32_t tok0, tok1;     /* first token; token after the last one */
    SrcLoc loc;              /* the first specifier */
    uint8_t sc;              /* StorageClass */
    uint8_t word;            /* TypeWord */
    uint8_t kind;            /* TypeSpecKind */
    bool thread, thread_gnu, is_inline, is_noreturn;
    bool is_long, long_long, is_short, is_signed, is_unsigned, is_complex;
    bool default_int;        /* no type specifier at all */
    bool non_sc_seen;
    bool restrict_q, alignas_seen, has_attrs;
    bool has_type;           /* the type came whole (typedef, tag, typeof) */
    bool error;              /* the base type is erroneous */
    unsigned quals;          /* TQ_* written in the specifiers */
    uint32_t align;          /* _Alignas / aligned attribute, bytes */
    const char *wname;       /* TW_FLOATN / TW_DECIMAL: its name */
    uint32_t qual_tok[4];    /* token + 1 of the last const/volatile/restrict/
                                _Atomic */
    uint32_t tag_node;       /* the STRUCT/ENUM node giving the type, or NO_NODE */
    uint32_t nty;            /* TW_INT128 / TW_FLOATN / TW_DECIMAL: the type */
    uint32_t xref_name;      /* a pending "'S' defined as wrong kind of tag" */
    SrcLoc xref_loc;
    Attrs attrs;
    SrcLoc sc_loc, inline_loc, noreturn_loc, thread_loc, qual_loc, type_loc,
        complex_loc, alignas_loc;
} Spec;

/* ---- records being defined ------------------------------------------- */

typedef struct RecDef {
    TypeId ty;
    uint32_t first;          /* its fields in Checker.fields */
    uint32_t first_ec;       /* enums: its constants in Checker.ecs */
    bool is_enum;
    uint64_t next;           /* enums: the next implicit value */
    TypeId next_ty;          /* enums: its type */
    bool next_overflow;
    bool has_neg;
} RecDef;

/* ---- the checker ------------------------------------------------------ */

/* Constant kinds (Checker.ck). */
enum {
    K_NONE,                  /* not a constant */
    K_ERR,                   /* unknown: an error was already reported */
    K_ICE,                   /* integer constant expression (C99 6.6p6) */
    K_FOLD,                  /* integer value gcc folds, not an ICE */
    K_FLOAT,                 /* arithmetic constant: Checker.fv[cv] */
    K_ADDR                   /* address constant: cb (base) + cv (offset) */
};

/* Expression flags (Checker.ef). */
enum {
    EF_LVALUE = 1,
    EF_BITFIELD = 2,
    EF_NPC = 4,              /* null pointer constant */
    EF_REGISTER = 8,         /* names a register object */
    EF_STRING = 16,          /* a string literal (array initializers) */
    /* cexpr.c's bookkeeping (gcc's tree codes, as far as they matter) */
    EF_OVERFLOW = 32,        /* the value overflowed (TREE_OVERFLOW) */
    EF_INTOPS = 64,          /* integer constant operands only: may appear
                                in an unevaluated part of an ICE */
    EF_REALCST = 128,        /* a floating constant, possibly parenthesized */
    EF_ADDRLV = 256,         /* an lvalue at a constant address: cb + cv */
    EF_SIDE = 512,           /* has side effects (or reads a volatile) */
    EF_DECIMAL = 1024,       /* a decimal integer constant (-Wxor-used-as-pow) */
    EF_CST = 2048,           /* K_FOLD: a folded INTEGER_CST, not an ICE */
    EF_BFPROMOTE = 4096,     /* a bit-field narrower than int */
    EF_NOPCST = 8192,        /* K_FOLD: an INTEGER_CST under a conversion */
    EF_FOLDWARN = 16384      /* a warning is due when the full expression is
                                folded (gcc's c_fully_fold) */
};

/* Checker.cb for addresses not of a symbol: the node (string literal,
 * compound literal, &&label) with this bit. */
#define CB_NODE 0x40000000u

struct Checker {
    SrcMgr *sm;
    Interner *in;
    DiagEngine *diag;
    CheckOptions opt;
    const Target *tgt;
    TypeTable tt;

    /* symbols */
    VEC(CSym) gsyms;         /* persistent */
    VEC(CSym) lsyms;         /* this unit's */
    BindVec log;
    uint32_t *top[2];        /* by ident: innermost binding + 1 */
    uint32_t *ext;           /* by ident: external declaration + 1 */
    uint32_t nidents;        /* size of top[] and ext[] */
    VEC(ScopeMark) scopes;

    /* the unit */
    const ParseUnit *u;
    const Node *nodes;
    uint32_t nn;
    bool quiet;              /* the unit has syntax errors */
    TypeId *ty;              /* per node */
    uint64_t *cv;
    uint32_t *cb;            /* K_ADDR: base symbol ref + 1, 0: null */
    uint8_t *ck;
    uint16_t *ef;
    uint32_t *par;
    uint32_t cap;            /* capacity of the per-node arrays */
    VEC(long double) fv;     /* K_FLOAT values */
    VEC(Spec) specs;         /* the specifier stack */
    VEC(RecDef) recs;        /* records and enums being defined */
    VEC(FieldIn) fields;
    VEC(uint32_t) ecs;       /* enumeration constants being defined */
    BindVec saved;         /* bindings of closed prototype scopes */
    VEC(uint32_t) stack;     /* scratch */
    unsigned pack;           /* #pragma pack, bytes; 0: none */
    VEC(unsigned) pack_stack;
    uint32_t func_sym;       /* the function being defined, SYM_NONE */
    uint32_t cur_func_node;
    uint32_t cur_node;      /* the node being visited (NO_NODE: none) */
    uint32_t func_node;      /* the FUNC declarator of the definition */
    VEC(uint32_t) nested_undef; /* block-scope auto functions declared */
    VEC(uint32_t) tentative; /* file-scope objects to check at the end */
    SrcLoc last_bol;         /* the last token that began a line */
    /* cdecl.c: the latest place gcc's parser set input_location to a token
     * itself (a tag name, an enumerator): the token's index in the TU's
     * stream + 1; valid if greater than u->first_tok */
    uint64_t iloc_tok;
    /* cdecl.c: the parser's lookahead token at the diagnostic being made */
    uint32_t cd_ltok;
    /* cdecl.c: the prototype a K&R definition is checked against (gcc's
     * current_function_prototype_*) */
    TypeId cd_proto;
    SrcLoc cd_proto_loc;
    bool cd_have_proto;
    /* cexpr.c: identifiers already reported undeclared in the function
     * undecl_key names; whether the once-per-TU note was given; nodes
     * flagged EF_FOLDWARN not yet reported */
    VEC(uint32_t) undecl;
    uint64_t undecl_key;
    bool undecl_noted;
    uint32_t fold_pending;
    StrBuf esb[2];           /* cexpr.c: %E buffers (check.c frees) */
    unsigned enext, vnext;
    char vbuf[2][32];
    StrBuf sb;
};

/* ---- shared helpers --------------------------------------------------- */

static inline const Node *cnode(const Checker *c, uint32_t i)
{
    return &c->nodes[i];
}

/* A token's location as gcc's diagnostics report it: the spelling location
 * (a macro body's token at its definition, an argument at the use site). */
static inline SrcLoc ctok_loc(const Checker *c, uint32_t tok)
{
    const PTok *t = &c->u->toks[tok];
    return t->t.loc;
}
static inline SrcLoc cnode_loc(const Checker *c, uint32_t i)
{
    return ctok_loc(c, c->nodes[i].tok);
}
static inline const Tok *cnode_tok(const Checker *c, uint32_t i)
{
    return &c->u->toks[c->nodes[i].tok].t;
}
static inline uint32_t cnode_ident(const Checker *c, uint32_t i)
{
    const Tok *t = cnode_tok(c, i);
    return t->kind == TK_IDENT ? t->aux : 0;
}
static inline const char *cident(const Checker *c, uint32_t id)
{
    return ident_by_id(c->in, id)->str;
}
static inline CSym *csym(Checker *c, uint32_t ref)
{
    return ref & SYM_LOCAL ? &c->lsyms.data[ref & ~SYM_LOCAL]
                           : &c->gsyms.data[ref];
}
/* The first node of i's subtree. */
static inline uint32_t cfirst(const Checker *c, uint32_t i)
{
    return i + 1 - c->nodes[i].size;
}
static inline bool is_declarator_tag(unsigned tag)
{
    return tag == N_NAME || tag == N_PTR || tag == N_ARRAY || tag == N_FUNC;
}

void cerror(Checker *c, SrcLoc loc, const char *fmt, ...);
void cwarn(Checker *c, SrcLoc loc, const char *id, const char *fmt, ...);
/* A pedwarn: a warning (an error with -pedantic-errors), enabled when its
 * option id is ("" / NULL: always). */
Diagnostic *cpedwarn(Checker *c, SrcLoc loc, const char *id,
                     const char *fmt, ...);
/* A pedwarn under -Wpedantic only ([-Wpedantic]). */
Diagnostic *cpedantic(Checker *c, SrcLoc loc, const char *fmt, ...);
Diagnostic *cerror_d(Checker *c, SrcLoc loc, const char *fmt, ...);
Diagnostic *cwarn_d(Checker *c, DiagLevel lvl, SrcLoc loc, const char *id,
                    const char *fmt, ...);
void cnote(Checker *c, Diagnostic *d, SrcLoc loc, const char *fmt, ...);

/* gcc's input_location while the parser looks at token tok: the first
 * token of that token's line. */
SrcLoc cinput_loc(Checker *c, uint32_t tok);
/* The first ';' at nesting depth 0 from token tok. */
uint32_t cfind_semi(Checker *c, uint32_t tok);

/* Symbols and scopes (check.c). */
uint32_t csym_new(Checker *c, bool global, const CSym *s);
void cbind(Checker *c, int ns, uint32_t ident, uint32_t ref);
/* The innermost binding's ref: SYM_NONE (NS_ORD) / 0 (NS_TAG) if none. */
uint32_t clookup(Checker *c, int ns, uint32_t ident);
uint32_t lookup_ord(Checker *c, uint32_t ident);   /* SYM_NONE if none */
/* The binding (log index + 1) of ident in the innermost scope, 0 if none. */
uint32_t cbound_here(Checker *c, int ns, uint32_t ident);
ScopeKind cbind_scope(Checker *c, uint32_t bind);
void cscope_push(Checker *c, ScopeKind kind);
void cscope_pop(Checker *c, BindVec *save);
bool cat_file_scope(Checker *c);
ScopeKind cscope_kind(Checker *c);

/* Declarations (cdecl.c). */
void cdecl_node(Checker *c, uint32_t i);        /* every non-expression tag */
/* SCOPE[NF_PARAMS]: the parameters of the function being defined again. */
void cdecl_body_scope(Checker *c, uint32_t scope);
/* The SCOPE_END closing a function definition's scope. */
void cdecl_func_end(Checker *c, uint32_t scope_end);
/* End of the TU: one entry of Checker.tentative. */
void cdecl_finish_object(Checker *c, uint32_t ref);
TypeId type_of_typename(Checker *c, uint32_t i);   /* a TYPE_NAME node */

/* Expressions (cexpr.c). */
void cexpr_node(Checker *c, uint32_t i);
bool cexpr_is_expr(unsigned tag);
/* The type of an expression after lvalue conversion (arrays and functions
 * decay, qualifiers dropped). */
TypeId cexpr_rvalue_type(Checker *c, uint32_t i);
/* An integer constant's value as signed (per its type). */
int64_t cexpr_sval(Checker *c, uint32_t i);
/* Truncates v to type t (two's complement, sign-extended if signed). */
uint64_t cexpr_trunc(Checker *c, TypeId t, uint64_t v);
/* Does the value v of type from fit type to? */
bool cexpr_fits(Checker *c, uint64_t v, TypeId from, TypeId to);
/* The member name of a record (anonymous members searched); returns the
 * field's type and adds its offset in bits; false if there is none. */
bool cexpr_find_member(Checker *c, TypeId rec, uint32_t name, TypeId *ty,
                       uint64_t *off_bits, bool *bitfield);
/* Is node i inside __extension__ (pedantic warnings off)? */
bool cexpr_in_extension(Checker *c, uint32_t i);

#endif
