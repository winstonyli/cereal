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
};

const IaBt *ia32_find(const char *name)
{
    size_t k;
    for (k = 0; k < sizeof cbuiltin_x86_tab / sizeof *cbuiltin_x86_tab; k++)
        if (!strcmp(name, cbuiltin_x86_tab[k].bt.name))
            return &cbuiltin_x86_tab[k];
    return NULL;
}
