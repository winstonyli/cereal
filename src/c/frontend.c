/* frontend.c - parse and check one translation unit: the part shared by
 * `cereal parse|check|-fsyntax-only` and the language server's second
 * phase. */
#include "c/frontend.h"
#include "cell.h"
#include "toks.h"
#include "c/check.h"
#include "c/parse.h"

#include <string.h>

static bool pp_source(void *ctx, Tok *t, SrcLoc *exp_loc, SrcLoc *mloc)
{
    PP *pp = ctx;
    while (pp_next(pp, t))
        if (tok_in_stream(t)) {
            *exp_loc = t->kind == TK_PRAGMA ? t->loc : pp->out_exp_loc;
            *mloc = t->kind == TK_PRAGMA ? 0 : pp->out_mloc;
            return true;
        }
    return false;
}

/* Tokens regenerated from a cell build, cell by cell. */
typedef struct CellSource {
    TokRegen *rg;
    size_t cell;
    TokCursor cur;
    bool open;
    SrcLoc last_bol;            /* first token of the last line any cell read */
} CellSource;

static SrcLoc cell_last_line(void *ctx)
{
    return ((CellSource *)ctx)->last_bol;
}

/* The macros at the cursor: the open cell reads them at its own version. */
static void cell_macro_names(void *ctx, void (*cb)(void *, const char *, size_t),
                             void *arg)
{
    pp_macro_names(&((CellSource *)ctx)->cur.pp, cb, arg);
}

static bool cell_source(void *ctx, Tok *t, SrcLoc *exp_loc, SrcLoc *mloc)
{
    CellSource *cs = ctx;
    for (;;) {
        if (cs->open) {
            if (tokcur_next(&cs->cur, t, exp_loc, mloc))
                return true;
            if (cs->cur.pp.last_bol)
                cs->last_bol = cs->cur.pp.last_bol;
            tokcur_close(&cs->cur, true); /* the parser holds tokens */
            cs->open = false;
            cs->cell++;
        }
        if (cs->cell >= cs->rg->ncells)
            return false;
        tokcur_open(&cs->cur, cs->rg, cs->rg->cells[cs->cell].s,
                    cs->rg->cells[cs->cell].e);
        cs->open = true;
    }
}

/* Whether a lexer diagnostic held back by --cells precedes the token lim in
 * lex order.  Locations of an included file are numbered after the main
 * file's, so compare an included file's diagnostics by where its (outermost)
 * #include sits: lm is the last main-file token before lim. */
static bool pre_before(SrcMgr *sm, const SrcFile *mf, const Diagnostic *d,
                       SrcLoc lim, SrcLoc lm)
{
    bool dmain = d->loc >= mf->base && d->loc < mf->base + mf->span;
    bool lmain = lim >= mf->base && lim < mf->base + mf->span;
    SrcLoc k = !dmain && d->ninc ? d->inc_chain[d->ninc - 1] : d->loc;
    if (lmain)
        return k < lim;
    if (dmain)
        return d->loc <= lm;
    if (k <= lm)
        return true;
    {
        SrcFile *df = srcmgr_file_of(sm, d->loc), *lf = srcmgr_file_of(sm, lim);
        if (!df || !lf)
            return d->loc < lim;
        return df == lf ? d->loc < lim : df->base < lf->base;
    }
}

bool frontend_run(TU *tu, const char *path, const FrontendOpts *fo)
{
    Options *o = tu->opt;
    FILE *err = tu->diag.out, *out = fo->out;
    const uint32_t *cancel = tu->pp.cancel;
    Parser p;
    ParseUnit u;
    CellCache cache;
    TokRegen rg;
    CellSource cs;
    ParseSource src = pp_source;
    void *ctx = &tu->pp;
    Checker *chk = NULL;
    uint64_t errs;
    size_t mark, mid;
    Diagnostic **pre = NULL;
    size_t npre = 0, ipre = 0;
    SrcLoc lastm = 0;
    bool opened = false, cancelled;
    cell_cache_init(&cache);
    tokregen_init(&rg);
    memset(&cs, 0, sizeof cs);
    if (fo->cells_par) { /* the parser's input as the language server has it */
        ParOptions po = *fo->cells_par;
        ParResult r;
        po.force = true;
        po.cells = &cache;
        po.toks = &rg;
        r = par_run(tu, path, NULL, false, &po, NULL, 0);
        if (r == PAR_DONE && rg.ncells) {
            cs.rg = &rg;
            src = cell_source;
            ctx = &cs;
        } else if (r == PAR_FAILED) {
            goto done;
        } else {
            tokregen_free(&rg);
            tokregen_init(&rg);
            tu_free(tu);
            tu_init(tu, o);
            tu->diag.out = err;
            if (!tu_begin(tu, path))
                goto done;
        }
    } else if (!tu_begin(tu, path)) {
        goto done;
    }
    opened = true;
    parser_init(&p, &tu->sm, tu->in, &tu->diag, o->pp.gnu_mode, src, ctx);
    p.macro_chain = pp_macro_chain;
    p.macro_ctx = &tu->pp;
    if (src == pp_source) {
        p.last_line = pp_last_line;
        p.last_ctx = &tu->pp;
    } else {
        p.last_line = cell_last_line;
        p.last_ctx = &cs;
    }
    if (fo->check) {
        CheckOptions co;
        memset(&co, 0, sizeof co);
        co.target = fo->target;
        co.gnu = o->pp.gnu_mode;
        co.std_year = o->std_year;
        co.short_enums = o->short_enums;
        co.opt_level = o->opt_level;
        co.strict_alias = o->strict_alias;
        co.lax_vector = o->lax_vector;
        co.cf_nobranch = o->cf_nobranch;
        co.optimize = o->opt_level && o->opt_level != '0' &&
                      o->opt_level != 'g';
        co.pedantic = tu->diag.pedantic;
        co.pedantic_errors = tu->diag.pedantic_errors;
        co.dump = fo->dump_types ? out : NULL;
        co.dump_summaries = fo->dump_summaries ? out : NULL;
        co.validate_summaries = fo->validate_summaries;
        co.summaries = fo->keep_summaries;
        co.macro_chain = pp_macro_chain;
        co.macro_ctx = &tu->pp;
        co.macro_names = src == pp_source ? pp_macro_names : cell_macro_names;
        co.macro_names_ctx = src == pp_source ? (void *)&tu->pp : (void *)&cs;
        chk = checker_new(&tu->sm, tu->in, &tu->diag, &co);
    }
    /* From cells the whole file is lexed before the first unit, so its lexer
     * and preprocessor diagnostics are already reported; hold them back and
     * release each with the unit whose tokens (and one of lookahead) reach
     * it, where a sequential run lexes them. */
    if (src == cell_source && tu->diag.all.len) {
        npre = tu->diag.all.len;
        pre = xmalloc(npre * sizeof *pre);
        memcpy(pre, tu->diag.all.data, npre * sizeof *pre);
        tu->diag.all.len = 0;
    }
    for (errs = p.errors - p.soft_errors, mark = tu->diag.all.len;
         parser_next(&p, &u);
         errs = p.errors - p.soft_errors, mark = tu->diag.all.len) {
        if (fo->dump)
            ast_dump(out, &u, &tu->sm, tu->in);
        if (ipre < npre) {
            SrcLoc lim = p.toks.len > p.unit_end
                             ? p.toks.data[p.unit_end].t.loc
                             : p.unit_end ? p.toks.data[p.unit_end - 1].t.loc +
                                                p.toks.data[p.unit_end - 1].t.len
                                          : 0;
            size_t nrel = 0, cnt = tu->diag.all.len - mark, q;
            const SrcFile *mf = NULL;
            uint32_t ui, back;
            for (ui = 0; ui < srcmgr_nfiles(&tu->sm) && !mf; ui++)
                if (srcmgr_file(&tu->sm, ui)->kind == SF_USER)
                    mf = srcmgr_file(&tu->sm, ui);
            for (back = p.unit_end; mf && back-- > 0 && p.unit_end - back < 4096;)
                if (p.toks.data[back].t.loc >= mf->base &&
                    p.toks.data[back].t.loc < mf->base + mf->span) {
                    lastm = p.toks.data[back].t.loc;
                    break;
                }
            while (ipre + nrel < npre &&
                   pre_before(&tu->sm, mf, pre[ipre + nrel], lim, lastm))
                nrel++;
            for (q = 0; q < nrel; q++)
                vec_push(&tu->diag.all, NULL);
            memmove(tu->diag.all.data + mark + nrel, tu->diag.all.data + mark,
                    cnt * sizeof *pre);
            memcpy(tu->diag.all.data + mark, pre + ipre, nrel * sizeof *pre);
            ipre += nrel;
        }
        mid = tu->diag.all.len;
        if (cancel && atomic_load_u32(cancel))
            break; /* the preprocessor has stopped; the rest is stale */
        if (chk)
            checker_unit(chk, &u, p.errors - p.soft_errors > errs);
        if (mid > mark)
            diag_merge_from(&tu->diag, mark, mid);
    }
    for (; ipre < npre; ipre++)
        vec_push(&tu->diag.all, pre[ipre]);
    free(pre);
    cancelled = cancel && atomic_load_u32(cancel);
    if (chk && !cancelled && tu->diag.pedantic && p.units == 0 && p.base + p.unit_end == 0) {
        uint32_t k;
        for (k = 0; k < srcmgr_nfiles(&tu->sm); k++) {
            SrcFile *f = srcmgr_file(&tu->sm, k);
            if (f->kind == SF_VIRTUAL)
                continue;
            diag_report(&tu->diag,
                        tu->diag.pedantic_errors ? DL_ERROR : DL_WARNING,
                        "pedantic", f->base + f->size,
                        "ISO C forbids an empty translation unit");
            break;
        }
    }
    if (chk) {
        if (!cancelled)
            checker_finish(chk);
        checker_free(chk);
    }
    if (getenv("CEREAL_PARSE_STATS"))
        fprintf(stderr, "parse: %llu units, %llu tokens, %llu errors\n",
                (unsigned long long)p.units,
                (unsigned long long)(p.base + p.unit_end),
                (unsigned long long)p.errors);
    parser_free(&p);
    if (cs.open)
        tokcur_close(&cs.cur, true);
done:
    tokregen_free(&rg);
    cell_cache_free(&cache);
    return opened;
}
