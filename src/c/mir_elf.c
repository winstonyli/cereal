/* mir_elf.c - x86-64 machine code for mir.h and an ELF64 relocatable object.
 *
 * Encodings follow what GNU as picks for the AT&T text mir.c prints, so the
 * .text bytes can be compared with the assembler's (tests/asm_exec.py):
 * register-to-register moves and arithmetic use the "reg is the source"
 * opcodes, immediates take the 8-bit form when they fit (and the short
 * %rax form otherwise), displacements the shortest of 0/8/32 bits, and jumps
 * start short and grow until every one fits. */
#include <string.h>

#include "c/mir.h"

typedef struct Enc {
    uint8_t b[16];
    int n;
} Enc;

static void put(Enc *e, int v) { e->b[e->n++] = (uint8_t)v; }

static void put32(Enc *e, int64_t v)
{
    int i;
    for (i = 0; i < 4; i++)
        put(e, (int)((uint64_t)v >> (8 * i) & 0xff));
}

static int hw(int r)
{
    static const uint8_t t[R_GP_COUNT] = {0, 1, 2, 6, 7, 8, 9, 5, 4};
    return r >= R_X0 ? r - R_X0 : t[r];
}

static bool is_xmm(const MOp *o) { return o->k == MK_REG && o->reg >= R_X0; }

/* A byte register that needs a REX prefix to be spelled: spl, bpl, sil, dil. */
static bool rex_byte(int w, const MOp *o)
{
    return w == 1 && o->k == MK_REG && o->reg < R_X0 && hw(o->reg) >= 4 && hw(o->reg) < 8;
}

/* [legacy prefix] [REX] opcode modrm [sib] [disp]: `reg` is the ModRM reg
 * field (a register number or an opcode extension), `rm` a register or
 * memory operand. */
static void ins(Enc *e, int pfx, bool w8, bool force, const uint8_t *opc, int nopc, int reg,
                const MOp *rm)
{
    int rex = (w8 ? 8 : 0) | (reg >> 3 & 1) << 2, i;
    rex |= hw(rm->reg) >> 3 & 1;
    if (pfx)
        put(e, pfx);
    if (rex || force)
        put(e, 0x40 | rex);
    for (i = 0; i < nopc; i++)
        put(e, opc[i]);
    if (rm->k == MK_REG) {
        put(e, 0xc0 | (reg & 7) << 3 | (hw(rm->reg) & 7));
    } else {
        int b = hw(rm->reg) & 7, mod;
        int64_t d = rm->v;
        mod = d == 0 && b != 5 ? 0 : d >= -128 && d <= 127 ? 1 : 2;
        put(e, mod << 6 | (reg & 7) << 3 | b);
        if (b == 4)
            put(e, 0x24);
        if (mod == 1)
            put(e, (int)(d & 0xff));
        else if (mod == 2)
            put32(e, d);
    }
}

static void ins1(Enc *e, int pfx, bool w8, bool force, int opc, int reg, const MOp *rm)
{
    uint8_t o = (uint8_t)opc;
    ins(e, pfx, w8, force, &o, 1, reg, rm);
}

static void ins2(Enc *e, int pfx, bool w8, int opc, int reg, const MOp *rm)
{
    uint8_t o[2] = {0x0f, (uint8_t)opc};
    ins(e, pfx, w8, false, o, 2, reg, rm);
}

/* The x86 group-1 operations: opcode extension. */
static int arith_ext(int op)
{
    switch (op) {
    case M_ADD: return 0;
    case M_OR: return 1;
    case M_AND: return 4;
    case M_SUB: return 5;
    case M_XOR: return 6;
    case M_CMP: return 7;
    default: return -1;
    }
}

static int fpfx(const MInst *m) { return m->w == 4 ? 0xf3 : 0xf2; }

/* Everything but jumps; a call's rel32 is left zero (the relocation fills it). */
static bool enc_inst(const MInst *m, Enc *e)
{
    const MOp *a = &m->a, *b = &m->b;
    bool w8 = m->w == 8 || m->w == 0;
    int w = m->w ? m->w : 8, ext = arith_ext(m->op), pfx = w == 2 ? 0x66 : 0;
    e->n = 0;
    if (ext >= 0) {
        if (a->k == MK_IMM) {
            int64_t v = a->v;
            if (w == 1) {
                ins1(e, 0, false, rex_byte(w, b), 0x80, ext, b);
                put(e, (int)(v & 0xff));
            } else if (v >= -128 && v <= 127) {
                ins1(e, pfx, w8, false, 0x83, ext, b);
                put(e, (int)(v & 0xff));
            } else if (b->k == MK_REG && b->reg == R_AX) {
                if (w8)
                    put(e, 0x48);
                put(e, ext << 3 | 5);
                put32(e, v);
            } else {
                ins1(e, pfx, w8, false, 0x81, ext, b);
                put32(e, v);
            }
        } else if (a->k == MK_REG) {
            ins1(e, pfx, w8, rex_byte(w, a) || rex_byte(w, b), ext << 3 | (w == 1 ? 0 : 1),
                 hw(a->reg), b);
        } else {
            ins1(e, pfx, w8, rex_byte(w, b), ext << 3 | (w == 1 ? 2 : 3), hw(b->reg), a);
        }
        return true;
    }
    switch (m->op) {
    case M_MOV:
        if (a->k == MK_IMM) {
            if (w == 4 || w == 1) {
                int r = hw(b->reg);
                if (r >= 8)
                    put(e, 0x41);
                put(e, (w == 1 ? 0xb0 : 0xb8) + (r & 7));
                if (w == 1)
                    put(e, (int)(a->v & 0xff));
                else
                    put32(e, a->v);
            } else {
                ins1(e, 0, true, false, 0xc7, 0, b);
                put32(e, a->v);
            }
        } else if (a->k == MK_REG) {
            ins1(e, pfx, w8, rex_byte(w, a) || rex_byte(w, b), w == 1 ? 0x88 : 0x89, hw(a->reg), b);
        } else {
            ins1(e, pfx, w8, rex_byte(w, b), w == 1 ? 0x8a : 0x8b, hw(b->reg), a);
        }
        break;
    case M_MOVABS: {
        int r = hw(b->reg), i;
        put(e, 0x48 | (r >> 3));
        put(e, 0xb8 + (r & 7));
        for (i = 0; i < 8; i++)
            put(e, (int)((uint64_t)a->v >> (8 * i) & 0xff));
        break;
    }
    case M_MOVSX:
        if (w == 4)
            ins1(e, 0, true, false, 0x63, hw(b->reg), a);
        else
            ins2(e, 0, true, w == 1 ? 0xbe : 0xbf, hw(b->reg), a);
        break;
    case M_MOVZX:
        if (w == 4) {
            if (a->k == MK_REG)
                ins1(e, 0, false, false, 0x89, hw(a->reg), b);
            else
                ins1(e, 0, false, false, 0x8b, hw(b->reg), a);
        } else {
            ins2(e, 0, true, w == 1 ? 0xb6 : 0xb7, hw(b->reg), a);
        }
        break;
    case M_LEA:
        ins1(e, 0, true, false, 0x8d, hw(b->reg), a);
        break;
    case M_IMUL:
        if (a->k == MK_IMM) {
            bool s8 = a->v >= -128 && a->v <= 127;
            ins1(e, 0, true, false, s8 ? 0x6b : 0x69, hw(b->reg), b);
            if (s8)
                put(e, (int)(a->v & 0xff));
            else
                put32(e, a->v);
        } else {
            ins2(e, 0, true, 0xaf, hw(b->reg), a);
        }
        break;
    case M_SHL: ins1(e, 0, w8, false, 0xd3, 4, b); break;
    case M_SHR: ins1(e, 0, w8, false, 0xd3, 5, b); break;
    case M_SAR: ins1(e, 0, w8, false, 0xd3, 7, b); break;
    case M_NOT: ins1(e, pfx, w8, rex_byte(w, a), w == 1 ? 0xf6 : 0xf7, 2, a); break;
    case M_NEG: ins1(e, pfx, w8, rex_byte(w, a), w == 1 ? 0xf6 : 0xf7, 3, a); break;
    case M_DIV: ins1(e, pfx, w8, rex_byte(w, a), w == 1 ? 0xf6 : 0xf7, 6, a); break;
    case M_IDIV: ins1(e, pfx, w8, rex_byte(w, a), w == 1 ? 0xf6 : 0xf7, 7, a); break;
    case M_TEST:
        ins1(e, pfx, w8, rex_byte(w, a) || rex_byte(w, b), w == 1 ? 0x84 : 0x85, hw(a->reg), b);
        break;
    case M_SETCC: {
        static const uint8_t cc[CC_COUNT] = {4, 5, 0xc, 0xe, 0xf, 0xd, 2, 6, 7, 3, 0xa, 0xb};
        ins2(e, 0, false, 0x90 + cc[m->cc], 0, a);
        if (rex_byte(1, a)) {           /* insert the REX before the 0f */
            memmove(e->b + 1, e->b, (size_t)e->n);
            e->b[0] = 0x40;
            e->n++;
        }
        break;
    }
    case M_CQO: put(e, 0x48); put(e, 0x99); break;
    case M_PUSH: case M_POP: {
        int r = hw(a->reg);
        if (r >= 8)
            put(e, 0x41);
        put(e, (m->op == M_PUSH ? 0x50 : 0x58) + (r & 7));
        break;
    }
    case M_LEAVE: put(e, 0xc9); break;
    case M_RET: put(e, 0xc3); break;
    case M_CALL: put(e, 0xe8); put32(e, 0); break;
    case M_FMOV:
        if (b->k == MK_MEM)
            ins2(e, fpfx(m), false, 0x11, hw(a->reg), b);
        else
            ins2(e, fpfx(m), false, 0x10, hw(b->reg), a);
        break;
    case M_FADD: ins2(e, fpfx(m), false, 0x58, hw(b->reg), a); break;
    case M_FMUL: ins2(e, fpfx(m), false, 0x59, hw(b->reg), a); break;
    case M_FSUB: ins2(e, fpfx(m), false, 0x5c, hw(b->reg), a); break;
    case M_FDIV: ins2(e, fpfx(m), false, 0x5e, hw(b->reg), a); break;
    case M_FCMP: ins2(e, w == 4 ? 0 : 0x66, false, 0x2e, hw(b->reg), a); break;
    case M_CVTI2F: ins2(e, fpfx(m), true, 0x2a, hw(b->reg), a); break;
    case M_CVTF2I: ins2(e, fpfx(m), true, 0x2c, hw(b->reg), a); break;
    case M_CVTF2F: ins2(e, fpfx(m), false, 0x5a, hw(b->reg), a); break;
    case M_MOVQX:
        if (is_xmm(b))
            ins2(e, 0x66, w == 8, 0x6e, hw(b->reg), a);
        else
            ins2(e, 0x66, w == 8, 0x7e, hw(a->reg), b);
        break;
    case M_MOVAPS: ins2(e, 0, false, 0x28, hw(b->reg), a); break;
    default:
        return false;
    }
    return true;
}

/* ---- the object -------------------------------------------------------- */

typedef struct ObjFn {
    char *name;
    uint32_t off, size;
    bool global;
} ObjFn;

typedef struct ObjRel {
    uint32_t off;
    char *name;
} ObjRel;

struct MObj {
    VEC(uint8_t) text;
    VEC(ObjFn) fns;
    VEC(ObjRel) rels;
};

MObj *mobj_new(void) { return xcalloc(1, sizeof(MObj)); }

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

bool mobj_add(MObj *o, const MFunc *f)
{
    size_t n = f->code.len, i;
    Enc *enc = xcalloc(n ? n : 1, sizeof *enc);
    uint32_t *pos = xcalloc(n + 1, sizeof *pos), *lab;
    bool *lng = xcalloc(n ? n : 1, sizeof *lng), changed, ok = true;
    int64_t maxlab = 0;
    ObjFn fn;
    for (i = 0; i < n; i++) {
        const MInst *m = &f->code.data[i];
        if (m->op == M_LABEL && m->a.v > maxlab)
            maxlab = m->a.v;
        if (m->op != M_LABEL && m->op != M_JMP && m->op != M_JCC && !enc_inst(m, &enc[i]))
            ok = false;
    }
    lab = xcalloc((size_t)maxlab + 1, sizeof *lab);
    do {
        uint32_t p = 0;
        changed = false;
        for (i = 0; i < n; i++) {
            const MInst *m = &f->code.data[i];
            pos[i] = p;
            if (m->op == M_LABEL)
                lab[m->a.v] = p;
            else if (m->op == M_JMP)
                p += lng[i] ? 5 : 2;
            else if (m->op == M_JCC)
                p += lng[i] ? 6 : 2;
            else
                p += (uint32_t)enc[i].n;
        }
        pos[n] = p;
        for (i = 0; i < n; i++) {
            const MInst *m = &f->code.data[i];
            int64_t d;
            if ((m->op != M_JMP && m->op != M_JCC) || lng[i])
                continue;
            d = (int64_t)lab[m->a.v] - (int64_t)(pos[i] + 2);
            if (d < -128 || d > 127) {
                lng[i] = true;
                changed = true;
            }
        }
    } while (changed);
    fn.name = dup_str(f->name);
    fn.off = (uint32_t)o->text.len;
    fn.size = pos[n];
    fn.global = f->global;
    for (i = 0; ok && i < n; i++) {
        const MInst *m = &f->code.data[i];
        int k;
        if (m->op == M_LABEL)
            continue;
        if (m->op == M_JMP || m->op == M_JCC) {
            static const uint8_t cc[CC_COUNT] = {4, 5, 0xc, 0xe, 0xf, 0xd, 2, 6, 7, 3, 0xa, 0xb};
            int64_t d = (int64_t)lab[m->a.v] - (int64_t)(pos[i] + (lng[i] ? (m->op == M_JMP ? 5 : 6) : 2));
            Enc e;
            e.n = 0;
            if (!lng[i]) {
                put(&e, m->op == M_JMP ? 0xeb : 0x70 + cc[m->cc]);
                put(&e, (int)(d & 0xff));
            } else if (m->op == M_JMP) {
                put(&e, 0xe9);
                put32(&e, d);
            } else {
                put(&e, 0x0f);
                put(&e, 0x80 + cc[m->cc]);
                put32(&e, d);
            }
            for (k = 0; k < e.n; k++)
                vec_push(&o->text, e.b[k]);
            continue;
        }
        if (m->op == M_CALL) {
            ObjRel r;
            r.off = (uint32_t)o->text.len + 1;
            r.name = dup_str(m->a.name);
            vec_push(&o->rels, r);
        }
        for (k = 0; k < enc[i].n; k++)
            vec_push(&o->text, enc[i].b[k]);
    }
    if (ok)
        vec_push(&o->fns, fn);
    else
        free(fn.name);
    free(enc);
    free(pos);
    free(lab);
    free(lng);
    return ok;
}

/* ---- ELF64 -------------------------------------------------------------- */

typedef VEC(uint8_t) Bytes;

static void w8(Bytes *b, uint64_t v) { vec_push(b, (uint8_t)v); }
static void w16(Bytes *b, uint64_t v) { w8(b, v); w8(b, v >> 8); }
static void w32(Bytes *b, uint64_t v) { w16(b, v); w16(b, v >> 16); }
static void w64(Bytes *b, uint64_t v) { w32(b, v); w32(b, v >> 32); }

static void wstr(Bytes *b, const char *s)
{
    do
        w8(b, (uint8_t)*s);
    while (*s++);
}

static void align8(Bytes *b)
{
    while (b->len & 7)
        w8(b, 0);
}

static void shdr(Bytes *b, uint32_t name, uint32_t type, uint64_t flags, uint64_t off,
                 uint64_t size, uint32_t link, uint32_t info, uint64_t al, uint64_t ent)
{
    w32(b, name);
    w32(b, type);
    w64(b, flags);
    w64(b, 0);
    w64(b, off);
    w64(b, size);
    w32(b, link);
    w32(b, info);
    w64(b, al);
    w64(b, ent);
}

bool mobj_write(MObj *o, const char *path)
{
    Bytes img = {0}, strtab = {0}, symtab = {0}, rela = {0}, shstr = {0};
    VEC(char *) undef = {0};
    uint32_t *symidx;
    size_t i, j, nloc = 0;
    uint64_t off_text, off_rela, off_sym, off_str, off_shstr, off_note, off_sh;
    uint32_t n_text, n_rela, n_sym, n_str, n_shstr, n_note;
    bool ok;
    FILE *f;

    /* symbols: null, .text section, local functions, global functions, undefined */
    for (i = 0; i < o->fns.len; i++)
        nloc += !o->fns.data[i].global;
    for (i = 0; i < o->rels.len; i++) {
        bool known = false;
        for (j = 0; j < o->fns.len && !known; j++)
            known = !strcmp(o->fns.data[j].name, o->rels.data[i].name);
        for (j = 0; j < undef.len && !known; j++)
            known = !strcmp(undef.data[j], o->rels.data[i].name);
        if (!known)
            vec_push(&undef, o->rels.data[i].name);
    }
    symidx = xcalloc(o->fns.len + 1, sizeof *symidx);
    w8(&strtab, 0);
    w64(&symtab, 0);
    w64(&symtab, 0);
    w64(&symtab, 0);                    /* null symbol */
    w32(&symtab, 0);
    w8(&symtab, 3);                     /* STT_SECTION, local */
    w8(&symtab, 0);
    w16(&symtab, 1);
    w64(&symtab, 0);
    w64(&symtab, 0);
    for (int pass = 0; pass < 2; pass++)
        for (i = 0; i < o->fns.len; i++) {
            const ObjFn *fn = &o->fns.data[i];
            if (fn->global != (pass == 1))
                continue;
            symidx[i] = (uint32_t)(symtab.len / 24);
            w32(&symtab, (uint32_t)strtab.len);
            wstr(&strtab, fn->name);
            w8(&symtab, (fn->global ? 0x10 : 0) | 2);  /* STB_GLOBAL|LOCAL, STT_FUNC */
            w8(&symtab, 0);
            w16(&symtab, 1);
            w64(&symtab, fn->off);
            w64(&symtab, fn->size);
        }
    for (i = 0; i < undef.len; i++) {
        w32(&symtab, (uint32_t)strtab.len);
        wstr(&strtab, undef.data[i]);
        w8(&symtab, 0x10);              /* STB_GLOBAL, STT_NOTYPE */
        w8(&symtab, 0);
        w16(&symtab, 0);
        w64(&symtab, 0);
        w64(&symtab, 0);
    }
    for (i = 0; i < o->rels.len; i++) {
        uint32_t sym = 0;
        for (j = 0; j < o->fns.len; j++)
            if (!strcmp(o->fns.data[j].name, o->rels.data[i].name))
                sym = symidx[j];
        for (j = 0; !sym && j < undef.len; j++)
            if (!strcmp(undef.data[j], o->rels.data[i].name))
                sym = (uint32_t)(2 + o->fns.len + j);
        w64(&rela, o->rels.data[i].off);
        w64(&rela, (uint64_t)sym << 32 | 4);    /* R_X86_64_PLT32 */
        w64(&rela, (uint64_t)-4);
    }
    w8(&shstr, 0);
    {
        static const char *const names[] = {".text", ".rela.text", ".symtab", ".strtab",
                                            ".shstrtab", ".note.GNU-stack"};
        for (i = 0; i < 6; i++)
            wstr(&shstr, names[i]);
    }

    /* the image: header, section contents, section headers */
    for (i = 0; i < 64; i++)
        w8(&img, 0);
    off_text = img.len;
    n_text = (uint32_t)o->text.len;
    for (i = 0; i < o->text.len; i++)
        w8(&img, o->text.data[i]);
    align8(&img);
    off_rela = img.len;
    n_rela = (uint32_t)rela.len;
    for (i = 0; i < rela.len; i++)
        w8(&img, rela.data[i]);
    align8(&img);
    off_sym = img.len;
    n_sym = (uint32_t)symtab.len;
    for (i = 0; i < symtab.len; i++)
        w8(&img, symtab.data[i]);
    off_str = img.len;
    n_str = (uint32_t)strtab.len;
    for (i = 0; i < strtab.len; i++)
        w8(&img, strtab.data[i]);
    off_shstr = img.len;
    n_shstr = (uint32_t)shstr.len;
    for (i = 0; i < shstr.len; i++)
        w8(&img, shstr.data[i]);
    off_note = img.len;
    n_note = 0;
    align8(&img);
    off_sh = img.len;
    shdr(&img, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    shdr(&img, 1, 1, 6, off_text, n_text, 0, 0, 1, 0);                  /* .text: AX */
    shdr(&img, 7, 4, 0x40, off_rela, n_rela, 3, 1, 8, 24);              /* .rela.text */
    shdr(&img, 18, 2, 0, off_sym, n_sym, 4, (uint32_t)(2 + nloc), 8, 24);   /* .symtab */
    shdr(&img, 26, 3, 0, off_str, n_str, 0, 0, 1, 0);                   /* .strtab */
    shdr(&img, 34, 3, 0, off_shstr, n_shstr, 0, 0, 1, 0);               /* .shstrtab */
    shdr(&img, 44, 1, 0, off_note, n_note, 0, 0, 1, 0);                 /* .note.GNU-stack */
    {
        Bytes h = {0};
        static const uint8_t ident[16] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
        for (i = 0; i < 16; i++)
            w8(&h, ident[i]);
        w16(&h, 1);                     /* ET_REL */
        w16(&h, 62);                    /* EM_X86_64 */
        w32(&h, 1);
        w64(&h, 0);
        w64(&h, 0);
        w64(&h, off_sh);
        w32(&h, 0);
        w16(&h, 64);
        w16(&h, 0);
        w16(&h, 0);
        w16(&h, 64);
        w16(&h, 7);
        w16(&h, 5);
        memcpy(img.data, h.data, h.len);
        vec_free(&h);
    }
    f = fopen(path, "wb");
    ok = f && fwrite(img.data, 1, img.len, f) == img.len;
    if (f && fclose(f))
        ok = false;
    vec_free(&img);
    vec_free(&strtab);
    vec_free(&symtab);
    vec_free(&rela);
    vec_free(&shstr);
    vec_free(&undef);
    free(symidx);
    return ok;
}

void mobj_free(MObj *o)
{
    size_t i;
    if (!o)
        return;
    for (i = 0; i < o->fns.len; i++)
        free(o->fns.data[i].name);
    for (i = 0; i < o->rels.len; i++)
        free(o->rels.data[i].name);
    vec_free(&o->text);
    vec_free(&o->fns);
    vec_free(&o->rels);
    free(o);
}
