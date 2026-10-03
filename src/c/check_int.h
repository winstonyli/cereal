/* check_int.h - the checker's internals, shared by check.c (declarations)
 * and cexpr.c (expressions and constant evaluation). */
#ifndef CEREAL_CHECK_INT_H
#define CEREAL_CHECK_INT_H

#include "c/check.h"
#include "c/ckw.h"
#include "c/csum.h"
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
    CSF_DECL_EXTERNAL = 16384, /* gcc's DECL_EXTERNAL */
    CSF_TREE_STATIC = 32768, /* gcc's TREE_STATIC */
    CSF_AUTO_TYPE = 65536,   /* __auto_type: type comes from the initializer */
    CSF_ARRAY_PARM = 131072, /* a parameter declared with an array type */
    CSF_WEAK = 262144,       /* __attribute__((weak)) */
    CSF_ADDR_WARNED = 524288, /* -Waddress 'will always be true' given */
    CSF_CONST_INIT = 1048576, /* const scalar with a constant initializer */
    CSF_ATTR_UNUSED = 2097152, /* __attribute__((unused)) */
    CSF_FWD = 4194304,       /* parameter only forward-declared so far */
    CSF_DEPRECATED = 8388608, /* __attribute__((deprecated)) */
    CSF_UNAVAILABLE = 16777216, /* __attribute__((unavailable)) */
    CSF_INNER_COMP = 33554432, /* incomplete array completed in an inner scope */
    CSF_GNU_INLINE = 67108864, /* __attribute__((gnu_inline)) */
    CSF_PURE = 134217728,    /* __attribute__((pure)), possibly copied */
    CSF_CONSTFN = 268435456, /* __attribute__((const)), possibly copied */
    CSF_CONST_VAL = 536870912, /* -O: const integer, constant initializer in
                                 CSym.val (gcc's decl_constant_value) */
    CSF_WEAKREF = 1073741824 /* __attribute__((weakref)) */
};

typedef struct AName {
    uint32_t set;
    uint32_t prev;           /* the set's previous entry, 1-based; 0: none */
    char name[32];           /* without the __ affixes */
    char arg[24];            /* canonical arguments (cdecl_attr_args) */
} AName;

/* A reference to / declaration of a static in an inline definition of a
 * function with external linkage (C99 6.7.4p3), diagnosed at the end. */
typedef struct InlStatic {
    SrcLoc loc;
    uint32_t fref, name;
    bool modifiable;
} InlStatic;

typedef struct StrInit {
    char *b;                 /* the decoded bytes */
    size_t n;
    bool uns;                /* element type is not plain char */
} StrInit;

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
    uint32_t dep_msg;        /* deprecated/unavailable message: Checker.dep_msgs + 1 */
    uint32_t sect;           /* section(".."): Checker.dep_msgs + 1, 0: none */
    uint64_t nonnull;        /* the 'nonnull' attribute (Attrs.nonnull) */
    uint32_t fmt;            /* the 'format' attribute (Attrs.fmt) */
    uint8_t fmtarg;          /* the 'format_arg' attribute's argument number */
    uint32_t aset;           /* attribute names written or copied: Checker.anames set id */
    uint32_t ualign;         /* a function's user alignment in bytes (aligned attribute) */
    uint32_t strinit;        /* const char array initialised by a string: Checker.strinits + 1 */
    uint32_t parms;          /* cparm.c: how the parameters were declared (the
                              * first prototype's): Checker.pdescs + 1 */
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
    uint32_t ty;             /* NS_ORD: the type of the symbol in this scope
                              * when it differs from the symbol's, + 1 (gcc's
                              * c_binding.u.type) */
    uint64_t nn;             /* NS_ORD: 'nonnull' of a block-scope function pointer */
    uint8_t ns;
} Bind;

typedef VEC(Bind) BindVec;
typedef struct DiagState { DiagConfig *cfg; uint64_t dig; } DiagState;

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
#define NN_ALL ((uint64_t)1 << 63)

typedef struct Attrs {
    uint32_t aligned;        /* bytes, 0: none */
    bool packed;
    bool transparent_union;
    bool may_alias;
    bool has_mode;
    uint8_t mode_bytes;      /* mode(QI..TI): integer size; 0: word etc. */
    uint8_t nunk;            /* unknown attribute names deferred to the declarator */
    char unk[2][48];
    char mode_name[16];      /* the mode as written, for diagnostics */
    uint8_t mode_float;      /* mode(SF/DF/XF/TF): 1 */
    uint64_t vector_size;    /* bytes, 0: none (or invalid) */
    bool vs_seen;            /* a valid vector_size argument */
    SrcLoc vs_loc;           /* input_location when it was read */
    SrcLoc vs_dup;           /* a second vector_size (0: none): it applies to
                              * the vector type the first one made */
    int8_t ms;               /* ms_struct 1, gcc_struct -1 */
    bool deprecated, unused, noreturn, weak, alias, section, cleanup;
    uint32_t cleanup_arg;
    bool weakref, ifunc, errattr, warnattr, desig;
    bool defn;               /* alias/ifunc/weakref naming a target */
    bool e_wi, e_iw;         /* weak then ifunc, ifunc then weak (both errors) */
    bool noinline, used;     /* seen, for the 'attribute ignored' checks */
    bool unavailable, gnu_inline;
    uint8_t zcur;            /* zero_call_used_regs: 1 ok, 2 not a string, 3 unrecognized */
    char zcur_arg[24];
    uint64_t nonnull;        /* bit j: argument j + 1; NN_ALL: every pointer */
    uint32_t fmt;            /* format(printf|scanf, N, M): kind 1|2 << 24, N << 12, M */
    uint8_t fmtarg;          /* format_arg(N) */
    uint32_t dep_msg;        /* Checker.dep_msgs + 1, 0: none */
    uint32_t sec, sec2;      /* section("..") strings (Checker.dep_msgs + 1);
                              * sec2: a later one, compared with sec */
    bool sec_any;            /* a section attribute was written */
    bool wina;               /* warn_if_not_aligned was written */
    uint8_t sso;             /* scalar_storage_order: 1 big-endian, 2 little */
    SrcLoc sec_bad;          /* input_location of a non-string argument */
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
    EF_FOLDWARN = 16384,     /* a warning is due when the full expression is
                                folded (gcc's c_fully_fold) */
    EF_GCCFOLD = 32768,      /* a comparison gcc folds to a constant (only
                                the location of -Wunused-value changes) */
    EF_ZDIV = 65536,         /* contains an integer division by constant zero:
                                fold keeps it, so x * 0 is not folded away */
    EF_PROP = EF_SIDE | EF_ZDIV  /* what an operator inherits from operands */
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
    VEC(StrInit) strinits;   /* string initializers of const char arrays (cexpr.c) */
    VEC(char *) dep_msgs;    /* deprecated/unavailable attribute messages */
    VEC(uint32_t) tdvla;              /* cparm.c: (typedef type, pdescs index + 1) pairs */
    VEC(struct CParmDesc *) pdescs;   /* cparm.c: parameter declarations */
    VEC(AName) anames;       /* attribute names by set (__builtin_has_attribute, copy) */
    VEC(uint32_t) ahead;     /* per set: its newest entry in anames, 1-based */
    uint32_t nasets;
    VEC(uint32_t) tdas;      /* (typedef type, attribute set) pairs */
    char ign[8][24];         /* attributes the declaration being checked lost to an exclusion */
    unsigned nign;
    uint32_t last_ualign;    /* the declaration's user alignment, for the symbol */
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
    size_t *dm;              /* per node: diagnostics so far when visited */
    TypeId *ty;              /* per node */
    uint64_t *cv;
    uint32_t *cb;            /* K_ADDR: base symbol ref + 1, 0: null */
    uint8_t *ck;
    uint32_t *ef;
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
    const DiagConfig *diag_cfg0;  /* the command line's -W state while a pragma
                                     has changed it (c->diag->cfg) */
    DiagConfig *diag_cur;    /* #pragma GCC diagnostic applied; NULL: none */
    uint64_t diag_dig;       /* digest of that state */
    VEC(DiagState) diag_stack;
    uint32_t func_sym;       /* the function being defined, SYM_NONE */
    uint32_t lbl_fn, lbl_n, lbl_ids[64]; /* labels already warned about (-Waddress) */
    uint32_t cur_func_node;
    uint32_t cur_node;      /* the node being visited (NO_NODE: none) */
    uint32_t fwd_warned;     /* 1 + the FUNC whose forward-declaration pedwarn
                                was given (0: none) */
    uint32_t func_node;      /* the FUNC declarator of the definition */
    VEC(uint32_t) nested_undef; /* block-scope auto functions declared */
    VEC(uint32_t) tentative; /* file-scope objects to check at the end */
    VEC(InlStatic) inl_statics; /* statics seen in extern inline functions */
    SrcLoc last_bol;         /* the last token that began a line */
    /* cdecl.c: the latest place gcc's parser set input_location to a token
     * itself (a tag name, an enumerator): the token's index in the TU's
     * stream + 1; valid if greater than u->first_tok */
    uint64_t iloc_tok;
    /* cdecl.c: the parser's lookahead token at the diagnostic being made */
    uint32_t cd_ltok;
    uint64_t il_first;       /* cinput_loc memo: the unit (first_tok), token + 1, its BOL token + 1 (0: none) */
    uint32_t il_tok, il_bol;
    bool attr_defer;         /* record unknown names in Attrs.unk instead */
    uint32_t mn_idx;         /* ... and the token chosen for it */
    SrcLoc mn_loc;           /* ... and its location */
    SrcLoc mn_exp;           /* invocation of the last diagnostic with macro notes */
    bool dup_no_cond;        /* -Wduplicated-branches: ?: not comparable */
    uint64_t align_err_u;    /* the aligned() argument last diagnosed, so */
    uint32_t align_err_node; /* a specifier's attributes collected twice */
                             /* report it once */
    bool attr_quiet;         /* attr_collect emits no unknown-attribute warning */
    bool attr_at_set;        /* ... and locates it at attr_at */
    SrcLoc attr_at;
    TypeId attr_fty;         /* the function type attributes are being applied to (0: unknown) */
    uint32_t cd_clit;       /* '{' token of the compound literal being typed */
    /* cdecl.c: the prototype a K&R definition is checked against (gcc's
     * current_function_prototype_*) */
    TypeId cd_proto;
    SrcLoc cd_proto_loc;
    bool cd_have_proto;
    bool lax_noted;          /* the -flax-vector-conversions note was given */
    /* cexpr.c: identifiers already reported undeclared in the function
     * undecl_key names; whether the once-per-TU note was given; nodes
     * flagged EF_FOLDWARN not yet reported */
    VEC(uint32_t) undecl;
    uint64_t undecl_key;
    bool undecl_noted;
    uint32_t fold_pending;
    uint64_t fuzzy_work;     /* cexpr.c: spelling-suggestion effort spent */
    void *plocs;             /* cexpr.c: parameter locations of functions */
    uint64_t hdr_noted;      /* cexpr.c: headers a note already suggested */
    void *stmt;              /* cstmt.c: statement-level state */
    struct CSum *cs;         /* csum.c: summaries and read sets, or NULL */
    const CSumFast *csf;     /* csum.c: cs's read stamps */
    struct CInit *ci;        /* cinit.c: initializer state */
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
    return c->diag->track0 && t->exp ? t->exp : t->t.loc;
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
/* An identifier with extended characters as gcc prints it in the C locale:
 * each as \UXXXXXXXX (UCN or UTF-8 spelled alike). */
const char *cident_ucn(const char *s);

/* gcc reports a keyword extension at the keyword, before its operand is
 * parsed: moves the diagnostic added since n0 (if one was) in front of
 * those of node i's operand. */
void choist(Checker *c, uint32_t i, size_t n0);
void choist_at(Checker *c, size_t n0, size_t at);

static inline const char *cident(const Checker *c, uint32_t id)
{
    const Ident *i = ident_by_id(c->in, id);
    return i->ext ? cident_ucn(i->str) : i->str;
}

/* a parameter that is part of the function's type (not a GNU forward
 * declaration) */
#define is_real_param(c, n) \
    (ntag(c, n) == N_PARAM && !(cnode(c, n)->flags & NF_FWD))

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
/* constant_expression_warning: only under -pedantic, tagged -Woverflow */
void cconst_overflow(Checker *c, SrcLoc loc);
Diagnostic *cped11(Checker *c, SrcLoc loc, const char *fmt, ...);
bool cin_system(Checker *c, SrcLoc loc);
Diagnostic *cerror_d(Checker *c, SrcLoc loc, const char *fmt, ...);
Diagnostic *cwarn_d(Checker *c, DiagLevel lvl, SrcLoc loc, const char *id,
                    const char *fmt, ...);
void cnote(Checker *c, Diagnostic *d, SrcLoc loc, const char *fmt, ...);

/* gcc's input_location while the parser looks at token tok: the first
 * token of that token's line. */
SrcLoc cinput_loc(Checker *c, uint32_t tok);
uint32_t cbol_tok(Checker *c, uint32_t tok);   /* BOL token + 1 of tok's line (0: none); memoized */
/* The first ';' at nesting depth 0 from token tok. */
uint32_t cfind_semi(Checker *c, uint32_t tok);

/* Symbols and scopes (check.c). */
uint32_t csym_new(Checker *c, bool global, const CSym *s);
void cbind(Checker *c, int ns, uint32_t ident, uint32_t ref);
uint32_t cbind_type(Checker *c, uint32_t ident);  /* top binding's type + 1 */
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
bool cexpr_enum_int_mix(Checker *c, TypeId a, TypeId b, int depth);
void cdecl_record_inline_static(Checker *c, SrcLoc loc, uint32_t name,
                                bool modifiable);
void cdecl_check_inline_statics(Checker *c);
TypeId type_of_typename(Checker *c, uint32_t i);   /* a TYPE_NAME node */

/* Array parameters redeclared with other bounds (cparm.c). */
bool cdecl_aset_has(const Checker *c, uint32_t set, const char *name,
                    const char *arg);
void cdecl_attrs_names(Checker *c, uint32_t attr, uint32_t *set);
uint32_t cdecl_typedef_aset(const Checker *c, TypeId t);
void cdecl_attr_args(Checker *c, uint32_t item, char *out, size_t n);
void cdecl_attr_name(const char *s, char *out, size_t n);
unsigned cexpr_asets(Checker *c, uint32_t e, bool strip, uint32_t out[3]);
uint32_t cparm_make(Checker *c, uint32_t fnode);    /* a prototype's record + 1 */
void cparm_compare(Checker *c, uint32_t nw, uint32_t old);
void cparm_release(Checker *c, uint32_t d);
void cparm_free(Checker *c);
bool cparm_restrict(Checker *c, uint32_t d, uint32_t j);
/* The access attributes a prototype's VLA parameters imply: parameter ptr
 * (1-based) has its bound in parameter size; bloc: where that is declared. */
typedef struct {
    uint32_t ptr, size;
    SrcLoc bloc;
    bool bnamed;
} CImplied;       /* size 0: a [*] bound */
unsigned cparm_implied(Checker *c, uint32_t d, CImplied *out, unsigned max);
void cparm_typedef(Checker *c, uint32_t top, TypeId ty);
/* Declarator helpers cparm.c shares (cdecl.c). */
uint32_t cdecl_inner_decl(const Checker *c, uint32_t i);
uint32_t cdecl_array_size_node(Checker *c, uint32_t a);
unsigned cdecl_quals_of(Checker *c, uint32_t node);

/* Expressions (cexpr.c). */
void cexpr_node(Checker *c, uint32_t i);
void cexpr_asm(Checker *c, uint32_t i);
/* A use of a deprecated/unavailable declaration, at loc. */
void cdep_use(Checker *c, SrcLoc loc, const CSym *s, const SrcLoc *note);
/* The value of a string literal node, for an attribute message: its index
 * in Checker.dep_msgs + 1. */
uint32_t cdep_msg(Checker *c, uint32_t str_node);
void cdep_report(Checker *c, SrcLoc loc, uint32_t nameid, uint32_t flags,
                 uint32_t dep_msg, const SrcLoc *note);
void cdep_named(Checker *c, SrcLoc loc, const char *name, uint32_t flags,
                uint32_t dep_msg, const SrcLoc *note);   /* an asm statement's operands */
bool cexpr_is_expr(unsigned tag);
/* The type of an expression after lvalue conversion (arrays and functions
 * decay, qualifiers dropped). */
TypeId cexpr_rvalue_type(Checker *c, uint32_t i);
/* An operand used for its value: false (after an error) when its type is
 * incomplete. */
bool cexpr_rvalue_ok(Checker *c, uint32_t i);
bool cexpr_rvalue_ok_at(Checker *c, uint32_t i, SrcLoc loc);
/* An integer constant's value as signed (per its type). */
int64_t cexpr_sval(Checker *c, uint32_t i);
bool cexpr_bf_overflow(Checker *c, SrcLoc loc, uint32_t n, TypeId ft,
                       TypeId rt, unsigned w);
const char *cexpr_str(Checker *c, uint32_t i);
void cexpr_builtin_decl(Checker *c, const CSym *s);
void cexpr_builtin_noproto_fmt(Checker *c, const CSym *s, SrcLoc loc);
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
/* -Wc++-compat is on and node is not inside __extension__ */
bool cexpr_cxx_compat(Checker *c, uint32_t node);
bool cexpr_is_extension(Checker *c, uint32_t i);   /* an __extension__ unary */

/* Implicit conversion to an object's type (gcc's convert_for_assignment,
 * with its constraint errors and warnings): the value of expression node
 * `expr` (already checked: c->ty/ck/cv/ef are set) converted to type
 * `lhs`.  Used for assignment (cexpr.c), argument passing (cexpr.c),
 * initialization (cinit.c) and `return` (statements). */
enum {
    CONV_INIT,               /* "initialization of 'T' from 'U' ..." */
    CONV_ASSIGN,             /* "assignment to 'T' from 'U' ..." */
    CONV_ARG,                /* "passing argument N of 'f' ..." */
    CONV_RETURN              /* "returning 'U' from a function with ..." */
};

typedef struct ConvInfo {
    int context;             /* CONV_* */
    /* gcc's `location`: CONV_ASSIGN the '=' token; CONV_INIT the
     * initializer (its first token / its expr_loc for a brace-less value);
     * CONV_RETURN the `return` statement's location argument (gcc passes
     * the location of the 'return' keyword's operand: use what
     * c_finish_return receives).  0: the expression's own location
     * (expr_loc).  CONV_ARG ignores it (gcc uses the argument's
     * location). */
    SrcLoc loc;
    SrcLoc eloc;             /* CONV_ASSIGN: gcc's expr_loc (0: the first token of
                              * the expression) */
    /* CONV_ARG: the callee as printed by %qE ("f"), the 1-based argument
     * number, and where the "expected 'T' but argument is of type 'U'"
     * note points (the parameter's declaration; 0: the argument). */
    const char *fname;
    int parmnum;
    SrcLoc note_loc;
    /* CONV_ARG: instead of note_loc, the callee's symbol ref + 1 (the note
     * then goes to that function's parameter declaration, found when a
     * diagnostic needs it); 0: none. */
    uint32_t fsym;
    /* CONV_INIT inside aggregates: the text of gcc's spelling stack
     * ("a.b[2]"); after a diagnostic a note "(near initialization for
     * 'TEXT')" is added at `loc`.  NULL: none. */
    const char *near;
    /* CONV_ARG to an unprototyped builtin etc.: report as this warning
     * option instead of an error (NULL: normal). */
    const char *warnopt;
    unsigned lhs_bits;       /* the target is a bit-field of this width */
    bool lhs_bitfield;       /* the target is a bit-field (gcc's -Wc++-compat
                              * enum message then omits the types) */
} ConvInfo;

/* Checks the conversion of node expr (by gcc's rules for the context) to
 * lhs; reports as gcc does and returns false after an error (not after a
 * warning).  A void / erroneous expression or target type is an error /
 * silently fails.  The expression node may be any expression tag. */
/* cdecl.c: a function declarator was declared (declared: the DECLARED node;
 * ref: its symbol; def: a definition); records the locations of the
 * parameters for the notes of argument diagnostics. */
void cexpr_record_params(Checker *c, uint32_t declared, uint32_t ref, bool def);
void cexpr_free_params(Checker *c);
/* -Waddress for a pointer used as a truth value (loc: diagnostic location) */
void cexpr_truth_warn(Checker *c, uint32_t n, SrcLoc loc);
void cstmt_unit_begin(Checker *c);
void cstmt_free(Checker *c);
void cstmt_scope_open(Checker *c, uint32_t i);
void cstmt_scope_end(Checker *c, uint32_t i);
void cstmt_scope_end_post(Checker *c, uint32_t i);
void cstmt_enter(Checker *c, uint32_t i);
void cstmt_expr(Checker *c, uint32_t i);
bool cstmt_cond_identical(Checker *c, uint32_t i, bool immediate);
void cstmt_dup_branches(Checker *c, uint32_t scope, uint32_t end);
void cstmt_dup_cond(Checker *c, uint32_t scope, uint32_t end);
SrcLoc cexpr_colon_loc(Checker *c, uint32_t i, uint32_t mid, uint32_t els);
void cstmt_node(Checker *c, uint32_t i);
void cstmt_emit_labels(Checker *c, uint32_t scope_node, int64_t min_key);
void cexpr_cleanup_call(Checker *c, uint32_t fsym, TypeId vty, SrcLoc dloc,
                        SrcLoc il);
/* -Waddress-of-packed-member for a conversion cexpr_assign_check skips */
void cexpr_packed_check(Checker *c, uint32_t expr, TypeId to);
bool cexpr_assign_check(Checker *c, uint32_t expr, TypeId lhs,
                        const ConvInfo *ci);
/* The spelling suggestion for a misspelled member of rec (NULL: none). */
const char *cexpr_fuzzy_field(Checker *c, TypeId rec, uint32_t name);

/* Initializers (cinit.c): gcc's digest_init / process_init_element. */
/* A DECLARED node was visited: opens the context of a braced initializer. */
void cinit_declared(Checker *c, uint32_t declared);
/* An INIT_DECL is about to finish: checks a brace-less initializer. */
void cinit_decl_done(Checker *c, uint32_t idecl);
void cexpr_note_strinit(Checker *c, CSym *s, uint32_t init);
/* Before / after every node is visited (cinit.c follows the initializer
 * lists as the nodes arrive). */
void cinit_pre(Checker *c, uint32_t i);
void cinit_post(Checker *c, uint32_t i);
void cinit_reset(Checker *c);
void cinit_free(Checker *c);
/* cdecl.c: gcc's input_location while the parser looks at token tok. */
SrcLoc cdecl_iloc(Checker *c, uint32_t tok);

#endif
