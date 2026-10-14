/* indexjob.c - indexing one translation unit into the store (indexjob.h). */
#include "indexjob.h"

#include <string.h>

#include "c/cidxrec.h"

static const char *const STATUS[] = {"?", "OK", "PARTIAL", "OVER_LIMIT", "FAILED"};

const char *idx_status_name(int status)
{
    return status > 0 && status < (int)(sizeof STATUS / sizeof *STATUS) ? STATUS[status]
                                                                        : STATUS[0];
}

int idx_status_parse(const char *name)
{
    size_t i;
    for (i = 1; i < sizeof STATUS / sizeof *STATUS; i++)
        if (!strcmp(name, STATUS[i]))
            return (int)i;
    return 0;
}

void index_unit(const IdxJob *j, IdxOut *out)
{
    Options o;
    StrVec cfiles = {0};
    CRecSet set;
    CRecLimits lim = {j->max_bytes, j->cancel};
    const char *path = j->entry >= 0 ? j->cfg->cmds.data[j->entry].abs_file : j->path;
    CRecResult r;
    memset(out, 0, sizeof *out);
    options_init(&o);
    if (j->entry >= 0)
        config_options_entry(j->cfg, (size_t)j->entry, &o.rsp, &o, &cfiles);
    else
        config_options_in(j->cfg, path, &o.rsp, &o, &cfiles);
    r = crec_index(&o, path, &cfiles, j->build_id, &lim, &set);
    if (r == CREC_CANCELLED) {
        out->result = IDX_CANCELLED;
    } else if (r == CREC_UNOPENED) {
        out->result = IDX_ERROR;
        snprintf(out->msg, sizeof out->msg, "cannot open %s", path);
    } else {
        out->tukey = set.tukey;
        out->tu_status = set.status;
        if (j->dump)
            crec_dump(&set, j->cfg->root, j->dump);
        if (j->dir && !(j->retry_over && set.status == CTU_OVER_LIMIT) &&
            !crec_write(j->dir, &set, out->msg, sizeof out->msg))
            out->result = IDX_ERROR;
        crec_free(&set);
    }
    vec_free(&cfiles);
    options_free(&o);
}
