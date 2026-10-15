/* indexjob.c - indexing one translation unit into the store (indexjob.h). */
#include "indexjob.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

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

static void build_options(const IdxJob *j, const char *path, Options *o, StrVec *cfiles)
{
    options_init(o);
    if (j->entry >= 0)
        config_options_entry(j->cfg, (size_t)j->entry, &o->rsp, o, cfiles);
    else
        config_options_in(j->cfg, path, &o->rsp, o, cfiles);
}

static bool same_files(const StrVec *a, const CRecStatVec *b)
{
    size_t i;
    if (a->len != b->len)
        return false;
    for (i = 0; i < a->len; i++)
        if (strcmp(a->data[i], b->data[i].path))
            return false;
    return true;
}

/* Tests only (CEREAL_FAULT=index-cfg-race): a write that lands between the
 * snapshot and the read of a config file. */
static void test_append(const char *path)
{
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd >= 0) {
        if (write(fd, "\n", 1) != 1) {
            /* the test then fails by itself */
        }
        close(fd);
    }
}

/* Building the options reads the .cereal and response files, so their stat
 * data and hashes are taken before each build (7.2: stat before read) and the
 * options are built again until the files read are the ones snapshotted.  A
 * file that changes meanwhile then differs from its record at the next
 * sweep.  The snapshot's paths end up in the final options' strings. */
static void options_snapshot(const IdxJob *j, const char *path, Options *o, CRecStatVec *snap)
{
    StrVec cfiles = {0};
    VEC(char *) own = {0};    /* own: the snapshot's paths while the options they came from are rebuilt */
    int tries;
    size_t i;
    build_options(j, path, o, &cfiles);
    for (tries = 0; !same_files(&cfiles, snap); tries++) {
        vec_free(snap);
        crec_stat_files(&cfiles, snap);
        for (i = 0; i < own.len; i++)
            free(own.data[i]);
        own.len = 0;
        for (i = 0; i < snap->len; i++) {
            vec_push(&own, xstrdup(snap->data[i].path));
            snap->data[i].path = own.data[i];
        }
        if (tries == 4)                 /* the set keeps changing: take what there is */
            break;
        if (tries == 0 && fault_hit("index-cfg-race") && snap->len)
            test_append(snap->data[0].path);
        options_free(o);
        vec_free(&cfiles);
        build_options(j, path, o, &cfiles);
    }
    for (i = 0; i < snap->len; i++)
        snap->data[i].path = cfiles.data[i];
    for (i = 0; i < own.len; i++)
        free(own.data[i]);
    vec_free(&own);
    vec_free(&cfiles);
}

void index_unit(const IdxJob *j, IdxOut *out)
{
    Options o;
    CRecStatVec cfiles = {0};
    CRecSet set;
    CRecLimits lim = {j->max_bytes, j->cancel};
    const char *path = j->entry >= 0 ? j->cfg->cmds.data[j->entry].abs_file : j->path;
    CRecResult r;
    memset(out, 0, sizeof *out);
    options_snapshot(j, path, &o, &cfiles);
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
