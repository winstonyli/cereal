/* mir.c - the AT&T assembly printer for mir.h. */
#include "c/mir.h"

static const char *const reg_names[4][R_COUNT] = {
    /* 1 */ {"al", "cl", "dl", "sil", "dil", "r8b", "r9b", "bpl", "spl"},
    /* 2 */ {"ax", "cx", "dx", "si", "di", "r8w", "r9w", "bp", "sp"},
    /* 4 */ {"eax", "ecx", "edx", "esi", "edi", "r8d", "r9d", "ebp", "esp"},
    /* 8 */ {"rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "rbp", "rsp"},
};

static const char *const cc_names[CC_COUNT] = {"e", "ne", "l", "le", "g", "ge", "b", "be", "a", "ae",
                                               "p", "np"};

static const char *const op_names[M_COUNT] = {
#define X(n, s) s,
    MIR_OPS(X)
#undef X
};

static int wi(int w)
{
    return w == 1 ? 0 : w == 2 ? 1 : w == 4 ? 2 : 3;
}

static char suffix(int w)
{
    return "bwlq"[wi(w)];
}

static void put_op(const MFunc *f, const MOp *o, int w, FILE *out)
{
    switch (o->k) {
    case MK_REG:
        if (o->reg >= R_X0)
            fprintf(out, "%%xmm%d", o->reg - R_X0);
        else
            fprintf(out, "%%%s", reg_names[wi(w)][o->reg]);
        break;
    case MK_IMM:
        fprintf(out, "$%lld", (long long)o->v);
        break;
    case MK_MEM:
        if (o->v)
            fprintf(out, "%lld", (long long)o->v);
        fprintf(out, "(%%%s)", reg_names[3][o->reg]);
        break;
    case MK_LAB:
        fprintf(out, ".L%s_%lld", f->name, (long long)o->v);
        break;
    case MK_SYM:
        fprintf(out, "%s(%%rip)", o->name);
        break;
    }
}

static const char *const sec_dir[] = {".section .rodata", ".data", ".bss"};

void mir_print_data(const MData *d, FILE *out)
{
    size_t r = 0;
    uint64_t at = 0;
    fprintf(out, "\t%s\n\t.balign %u\n", sec_dir[d->sec], d->align);
    if (d->global)
        fprintf(out, "\t.globl %s\n", d->name);
    fprintf(out, "%s:\n", d->name);
    while (at < d->size) {
        uint64_t end = r < d->rels.len ? d->rels.data[r].off : d->size, k, z = 0;
        if (at == end && r < d->rels.len) {     /* an address */
            const MReloc *x = &d->rels.data[r++];
            fprintf(out, "\t.quad %s%+lld\n", x->name, (long long)x->addend);
            at += 8;
            continue;
        }
        for (k = at; k < end && (!d->bytes || !d->bytes[k]); k++)
            z++;
        if (z == end - at || z >= 16) {         /* a run of zeros */
            fprintf(out, "\t.zero %llu\n", (unsigned long long)z);
            at += z;
            continue;
        }
        fputs("\t.byte ", out);
        for (k = at; k < end && k < at + 16; k++)
            fprintf(out, "%s%u", k > at ? "," : "", d->bytes[k]);
        fputc('\n', out);
        at = k;
    }
}

void mdata_free(MData *d)
{
    size_t i;
    for (i = 0; i < d->rels.len; i++)
        free(d->rels.data[i].name);
    vec_free(&d->rels);
    free(d->name);
    free(d->bytes);
}

void mir_print(const MFunc *f, FILE *out)
{
    size_t i;
    fprintf(out, "\t.text\n");
    if (f->global)
        fprintf(out, "\t.globl %s\n", f->name);
    fprintf(out, "%s:\n", f->name);
    for (i = 0; i < f->code.len; i++) {
        const MInst *m = &f->code.data[i];
        int w = m->w ? m->w : 8;
        switch (m->op) {
        case M_LABEL:
            fprintf(out, ".L%s_%lld:\n", f->name, (long long)m->a.v);
            continue;
        case M_MOVSX:       /* movsbq, movswq, movslq: from width w to 64 bits */
            fprintf(out, "\tmovs%cq ", suffix(w));
            put_op(f, &m->a, w, out);
            fputc(',', out);
            put_op(f, &m->b, 8, out);
            break;
        case M_MOVZX:       /* movzbq, movzwq; 32 bits: movl %eax,%eax */
            if (w == 4) {
                fputs("\tmovl ", out);
                put_op(f, &m->a, 4, out);
                fputc(',', out);
                put_op(f, &m->b, 4, out);
            } else {
                fprintf(out, "\tmovz%cq ", suffix(w));
                put_op(f, &m->a, w, out);
                fputc(',', out);
                put_op(f, &m->b, 8, out);
            }
            break;
        case M_MOVABS:
            fputs("\tmovabsq ", out);
            put_op(f, &m->a, 8, out);
            fputc(',', out);
            put_op(f, &m->b, 8, out);
            break;
        case M_SETCC:
            fprintf(out, "\tset%s ", cc_names[m->cc]);
            put_op(f, &m->a, 1, out);
            break;
        case M_JCC:
            fprintf(out, "\tj%s ", cc_names[m->cc]);
            put_op(f, &m->a, 8, out);
            break;
        case M_JMP:
            fputs("\tjmp ", out);
            put_op(f, &m->a, 8, out);
            break;
        case M_CALL:
            fprintf(out, "\tcall %s@PLT", m->a.name);
            break;
        case M_CQO: case M_LEAVE: case M_RET:
            fprintf(out, "\t%s", op_names[m->op]);
            break;
        case M_PUSH: case M_POP: case M_NEG: case M_NOT: case M_IDIV: case M_DIV:
            fprintf(out, "\t%s%c ", op_names[m->op], suffix(w));
            put_op(f, &m->a, w, out);
            break;
        case M_FMOV: case M_FADD: case M_FSUB: case M_FMUL: case M_FDIV: case M_FCMP:
            fprintf(out, "\t%s%s ", op_names[m->op], w == 4 ? "ss" : "sd");
            put_op(f, &m->a, 8, out);
            fputc(',', out);
            put_op(f, &m->b, 8, out);
            break;
        case M_CVTI2F:
            fprintf(out, "\tcvtsi2%sq ", w == 4 ? "ss" : "sd");
            put_op(f, &m->a, 8, out);
            fputc(',', out);
            put_op(f, &m->b, 8, out);
            break;
        case M_CVTF2I:
            fprintf(out, "\tcvtt%s2siq ", w == 4 ? "ss" : "sd");
            put_op(f, &m->a, 8, out);
            fputc(',', out);
            put_op(f, &m->b, 8, out);
            break;
        case M_CVTF2F:
            fprintf(out, "\t%s ", w == 4 ? "cvtss2sd" : "cvtsd2ss");
            put_op(f, &m->a, 8, out);
            fputc(',', out);
            put_op(f, &m->b, 8, out);
            break;
        case M_MOVQX:       /* movq (64 bits) or movd (32) */
            fputs(w == 4 ? "\tmovd " : "\tmovq ", out);
            put_op(f, &m->a, w, out);
            fputc(',', out);
            put_op(f, &m->b, w, out);
            break;
        case M_MOVAPS:
            fputs("\tmovaps ", out);
            put_op(f, &m->a, 8, out);
            fputc(',', out);
            put_op(f, &m->b, 8, out);
            break;
        case M_SHL: case M_SAR: case M_SHR:     /* by %cl */
            fprintf(out, "\t%s%c %%cl,", op_names[m->op], suffix(w));
            put_op(f, &m->b, w, out);
            break;
        default:                                /* op a,b at width w */
            fprintf(out, "\t%s%c ", op_names[m->op], suffix(w));
            put_op(f, &m->a, w, out);
            fputc(',', out);
            put_op(f, &m->b, w, out);
            break;
        }
        fputc('\n', out);
    }
    fputs("\t.section .note.GNU-stack,\"\",@progbits\n", out);
}
