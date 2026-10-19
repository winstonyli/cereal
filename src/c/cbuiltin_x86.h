/* cbuiltin_x86.h - the x86 target built-ins (__builtin_ia32_*) the intrinsic headers of gcc
 * (emmintrin.h and friends) call: name without "__builtin_", the type "return|param|param..."
 * as gcc prints it (vectors as "__vector(bytes) element"), then how tast.c lowers a call to the
 * operations on the lanes of its vector arguments (ia32_call): the kind, the type of the lanes
 * the kind reads the operands as, and the kind's argument (see ia32_call).  gnu = 4: exists only
 * as __builtin_NAME. */
#define IA_V16QI "__vector(16) char"
#define IA_V8HI "__vector(16) short int"
#define IA_V4SI "__vector(16) int"
#define IA_V2DI "__vector(16) long long int"
#define IA_V4SF "__vector(16) float"
#define IA_V2DF "__vector(16) double"
#define IA_ROW(nm, sig, k, l, a) {{nm, "", 0, sig, 4}, k, l, a}
#define IA_BIN(nm, v, k, l, a) IA_ROW(nm, v "|" v "|" v, k, l, a)
#define IA_IMM(nm, v, k, l, a) IA_ROW(nm, v "|" v "|int", k, l, a)
#define IA_SHIFTS(nm, v, l, a) IA_IMM(nm "i128", v, IAK_SHIFT, l, a), IA_BIN(nm "128", v, IAK_SHIFT, l, (a) | 8)
static const IaBt cbuiltin_x86_tab[] = {
    IA_ROW("ia32_loaddqu", IA_V16QI "|const char *", IAK_LOAD, IAL_S8, 0),
    IA_ROW("ia32_storedqu", "void|char *|" IA_V16QI, IAK_STORE, IAL_S8, 0),
    IA_ROW("ia32_pmovmskb128", "int|" IA_V16QI, IAK_MOVMSK, IAL_S8, 0),
    IA_BIN("ia32_pandn128", IA_V2DI, IAK_ANDN, IAL_S64, 0),
    IA_BIN("ia32_pmaxub128", IA_V16QI, IAK_MINMAX, IAL_U8, 1),
    IA_BIN("ia32_pminub128", IA_V16QI, IAK_MINMAX, IAL_U8, 0),
    IA_BIN("ia32_pmaxsw128", IA_V8HI, IAK_MINMAX, IAL_S16, 1),
    IA_BIN("ia32_pminsw128", IA_V8HI, IAK_MINMAX, IAL_S16, 0),
    IA_BIN("ia32_paddsb128", IA_V16QI, IAK_SAT, IAL_S8, 0),
    IA_BIN("ia32_paddsw128", IA_V8HI, IAK_SAT, IAL_S16, 0),
    IA_BIN("ia32_paddusb128", IA_V16QI, IAK_SAT, IAL_U8, 0),
    IA_BIN("ia32_paddusw128", IA_V8HI, IAK_SAT, IAL_U16, 0),
    IA_BIN("ia32_psubsb128", IA_V16QI, IAK_SAT, IAL_S8, 1),
    IA_BIN("ia32_psubsw128", IA_V8HI, IAK_SAT, IAL_S16, 1),
    IA_BIN("ia32_psubusb128", IA_V16QI, IAK_SAT, IAL_U8, 1),
    IA_BIN("ia32_psubusw128", IA_V8HI, IAK_SAT, IAL_U16, 1),
    IA_BIN("ia32_pavgb128", IA_V16QI, IAK_AVG, IAL_U8, 0),
    IA_BIN("ia32_pavgw128", IA_V8HI, IAK_AVG, IAL_U16, 0),
    IA_BIN("ia32_pmulhw128", IA_V8HI, IAK_MULH, IAL_S16, 0),
    IA_BIN("ia32_pmulhuw128", IA_V8HI, IAK_MULH, IAL_U16, 0),
    IA_ROW("ia32_pmaddwd128", IA_V4SI "|" IA_V8HI "|" IA_V8HI, IAK_MADD, IAL_S16, 0),
    IA_ROW("ia32_pmuludq128", IA_V2DI "|" IA_V4SI "|" IA_V4SI, IAK_MULW, IAL_U32, 0),
    IA_ROW("ia32_psadbw128", IA_V2DI "|" IA_V16QI "|" IA_V16QI, IAK_SAD, IAL_U8, 0),
    IA_BIN("ia32_punpcklbw128", IA_V16QI, IAK_UNPACK, IAL_S8, 0),
    IA_BIN("ia32_punpckhbw128", IA_V16QI, IAK_UNPACK, IAL_S8, 1),
    IA_BIN("ia32_punpcklwd128", IA_V8HI, IAK_UNPACK, IAL_S16, 0),
    IA_BIN("ia32_punpckhwd128", IA_V8HI, IAK_UNPACK, IAL_S16, 1),
    IA_BIN("ia32_punpckldq128", IA_V4SI, IAK_UNPACK, IAL_S32, 0),
    IA_BIN("ia32_punpckhdq128", IA_V4SI, IAK_UNPACK, IAL_S32, 1),
    IA_BIN("ia32_punpcklqdq128", IA_V2DI, IAK_UNPACK, IAL_S64, 0),
    IA_BIN("ia32_punpckhqdq128", IA_V2DI, IAK_UNPACK, IAL_S64, 1),
    IA_ROW("ia32_packsswb128", IA_V16QI "|" IA_V8HI "|" IA_V8HI, IAK_PACK, IAL_S16, 0),
    IA_ROW("ia32_packuswb128", IA_V16QI "|" IA_V8HI "|" IA_V8HI, IAK_PACK, IAL_S16, 1),
    IA_ROW("ia32_packssdw128", IA_V8HI "|" IA_V4SI "|" IA_V4SI, IAK_PACK, IAL_S32, 0),
    IA_SHIFTS("ia32_psllw", IA_V8HI, IAL_U16, 0),
    IA_SHIFTS("ia32_pslld", IA_V4SI, IAL_U32, 0),
    IA_SHIFTS("ia32_psllq", IA_V2DI, IAL_U64, 0),
    IA_SHIFTS("ia32_psrlw", IA_V8HI, IAL_U16, 1),
    IA_SHIFTS("ia32_psrld", IA_V4SI, IAL_U32, 1),
    IA_SHIFTS("ia32_psrlq", IA_V2DI, IAL_U64, 1),
    IA_SHIFTS("ia32_psraw", IA_V8HI, IAL_S16, 2),
    IA_SHIFTS("ia32_psrad", IA_V4SI, IAL_S32, 2),
    IA_IMM("ia32_pshufd", IA_V4SI, IAK_SHUF, IAL_S32, 0),
    IA_IMM("ia32_pshuflw", IA_V8HI, IAK_SHUF, IAL_S16, 1),
    IA_IMM("ia32_pshufhw", IA_V8HI, IAK_SHUF, IAL_S16, 2),
    IA_IMM("ia32_pslldqi128", IA_V2DI, IAK_BSHIFT, IAL_U8, 0),
    IA_IMM("ia32_psrldqi128", IA_V2DI, IAK_BSHIFT, IAL_U8, 1),
    IA_ROW("ia32_vec_ext_v4si", "int|" IA_V4SI "|int", IAK_EXT, IAL_S32, 0),
    IA_ROW("ia32_vec_ext_v8hi", "short int|" IA_V8HI "|int", IAK_EXT, IAL_S16, 0),
    IA_ROW("ia32_vec_set_v8hi", IA_V8HI "|" IA_V8HI "|short int|int", IAK_SET, IAL_S16, 0),
    IA_ROW("ia32_movq128", IA_V2DI "|" IA_V2DI, IAK_MOVQ, IAL_S64, 0),
    /* single and double precision */
#define IA_F2(nm, k, a) IA_BIN("ia32_" nm "ss", IA_V4SF, k, IAL_F32, a), IA_BIN("ia32_" nm "sd", IA_V2DF, k, IAL_F64, a)
    IA_F2("add", IAK_FSOP, 0), IA_F2("sub", IAK_FSOP, 1), IA_F2("mul", IAK_FSOP, 2), IA_F2("div", IAK_FSOP, 3),
    IA_F2("min", IAK_FSOP, 4), IA_F2("max", IAK_FSOP, 5),
    IA_BIN("ia32_minps", IA_V4SF, IAK_FPMM, IAL_F32, 4), IA_BIN("ia32_maxps", IA_V4SF, IAK_FPMM, IAL_F32, 5),
    IA_BIN("ia32_minpd", IA_V2DF, IAK_FPMM, IAL_F64, 4), IA_BIN("ia32_maxpd", IA_V2DF, IAK_FPMM, IAL_F64, 5),
    IA_BIN("ia32_andps", IA_V4SF, IAK_BITOP, IAL_S32, P_AMP), IA_BIN("ia32_andpd", IA_V2DF, IAK_BITOP, IAL_S64, P_AMP),
    IA_BIN("ia32_orps", IA_V4SF, IAK_BITOP, IAL_S32, P_PIPE), IA_BIN("ia32_orpd", IA_V2DF, IAK_BITOP, IAL_S64, P_PIPE),
    IA_BIN("ia32_xorps", IA_V4SF, IAK_BITOP, IAL_S32, P_CARET), IA_BIN("ia32_xorpd", IA_V2DF, IAK_BITOP, IAL_S64, P_CARET),
    IA_BIN("ia32_andnps", IA_V4SF, IAK_ANDN, IAL_S32, 0), IA_BIN("ia32_andnpd", IA_V2DF, IAK_ANDN, IAL_S64, 0),
#define IA_C(nm, p) \
    IA_BIN("ia32_cmp" nm "ps", IA_V4SF, IAK_FCMP, IAL_F32, p), IA_BIN("ia32_cmp" nm "pd", IA_V2DF, IAK_FCMP, IAL_F64, p), \
    IA_BIN("ia32_cmp" nm "ss", IA_V4SF, IAK_FCMP, IAL_F32, (p) | 16), IA_BIN("ia32_cmp" nm "sd", IA_V2DF, IAK_FCMP, IAL_F64, (p) | 16)
    IA_C("eq", 0), IA_C("lt", 1), IA_C("le", 2), IA_C("unord", 3), IA_C("neq", 4), IA_C("nlt", 5), IA_C("nle", 6),
    IA_C("ord", 7), IA_C("gt", 8), IA_C("ge", 9), IA_C("ngt", 10), IA_C("nge", 11),
#define IA_CI(nm, p) \
    IA_ROW("ia32_comi" nm, "int|" IA_V4SF "|" IA_V4SF, IAK_FCOMI, IAL_F32, p), \
    IA_ROW("ia32_ucomi" nm, "int|" IA_V4SF "|" IA_V4SF, IAK_FCOMI, IAL_F32, p), \
    IA_ROW("ia32_comisd" nm, "int|" IA_V2DF "|" IA_V2DF, IAK_FCOMI, IAL_F64, p), \
    IA_ROW("ia32_ucomisd" nm, "int|" IA_V2DF "|" IA_V2DF, IAK_FCOMI, IAL_F64, p)
    IA_CI("eq", P_EQEQ), IA_CI("lt", P_LT), IA_CI("le", P_LE), IA_CI("gt", P_GT), IA_CI("ge", P_GE), IA_CI("neq", P_NE),
    IA_ROW("ia32_movmskps", "int|" IA_V4SF, IAK_MOVMSK, IAL_S32, 0), IA_ROW("ia32_movmskpd", "int|" IA_V2DF, IAK_MOVMSK, IAL_S64, 0),
    IA_ROW("ia32_shufps", IA_V4SF "|" IA_V4SF "|" IA_V4SF "|int", IAK_SHUFP, IAL_S32, 0),
    IA_ROW("ia32_shufpd", IA_V2DF "|" IA_V2DF "|" IA_V2DF "|int", IAK_SHUFP, IAL_S64, 0),
    IA_BIN("ia32_unpcklps", IA_V4SF, IAK_UNPACK, IAL_S32, 0), IA_BIN("ia32_unpckhps", IA_V4SF, IAK_UNPACK, IAL_S32, 1),
    IA_BIN("ia32_unpcklpd", IA_V2DF, IAK_UNPACK, IAL_S64, 0), IA_BIN("ia32_unpckhpd", IA_V2DF, IAK_UNPACK, IAL_S64, 1),
    IA_BIN("ia32_movss", IA_V4SF, IAK_MOVLOW, IAL_S32, 0), IA_BIN("ia32_movsd", IA_V2DF, IAK_MOVLOW, IAL_S64, 0),
    IA_BIN("ia32_movhlps", IA_V4SF, IAK_MOVHL, IAL_S32, 0), IA_BIN("ia32_movlhps", IA_V4SF, IAK_MOVHL, IAL_S32, 1),
    IA_ROW("ia32_cvtdq2ps", IA_V4SF "|" IA_V4SI, IAK_CVT, IAL_S32, 4), IA_ROW("ia32_cvttps2dq", IA_V4SI "|" IA_V4SF, IAK_CVT, IAL_F32, 4),
    IA_ROW("ia32_cvtdq2pd", IA_V2DF "|" IA_V4SI, IAK_CVT, IAL_S32, 2), IA_ROW("ia32_cvtps2pd", IA_V2DF "|" IA_V4SF, IAK_CVT, IAL_F32, 2),
    IA_ROW("ia32_cvtpd2ps", IA_V4SF "|" IA_V2DF, IAK_CVT, IAL_F64, 2), IA_ROW("ia32_cvttpd2dq", IA_V4SI "|" IA_V2DF, IAK_CVT, IAL_F64, 2),
    IA_ROW("ia32_cvtsi2sd", IA_V2DF "|" IA_V2DF "|int", IAK_CVTS, IAL_F64, 0),
    IA_ROW("ia32_cvtsi642sd", IA_V2DF "|" IA_V2DF "|long long int", IAK_CVTS, IAL_F64, 0),
    IA_ROW("ia32_cvtsi2ss", IA_V4SF "|" IA_V4SF "|int", IAK_CVTS, IAL_F32, 0),
    IA_ROW("ia32_cvtsi642ss", IA_V4SF "|" IA_V4SF "|long long int", IAK_CVTS, IAL_F32, 0),
    IA_ROW("ia32_cvtsd2ss", IA_V4SF "|" IA_V4SF "|" IA_V2DF, IAK_CVTS, IAL_F32, 1 + IAL_F64),
    IA_ROW("ia32_cvtss2sd", IA_V2DF "|" IA_V2DF "|" IA_V4SF, IAK_CVTS, IAL_F64, 1 + IAL_F32),
    IA_ROW("ia32_cvttsd2si", "int|" IA_V2DF, IAK_EXT, IAL_F64, 0), IA_ROW("ia32_cvttsd2si64", "long long int|" IA_V2DF, IAK_EXT, IAL_F64, 0),
    IA_ROW("ia32_cvttss2si", "int|" IA_V4SF, IAK_EXT, IAL_F32, 0), IA_ROW("ia32_cvttss2si64", "long long int|" IA_V4SF, IAK_EXT, IAL_F32, 0),
};

const IaBt *ia32_find(const char *name)
{
    size_t k;
    for (k = 0; k < sizeof cbuiltin_x86_tab / sizeof *cbuiltin_x86_tab; k++)
        if (!strcmp(name, cbuiltin_x86_tab[k].bt.name))
            return &cbuiltin_x86_tab[k];
    return NULL;
}
