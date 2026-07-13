/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */

/**
 * Solve a single field
 *
 * Inputs: .ckdt .quad .skdt
 * Output: .match .rdls .wcs, ...
 */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <assert.h>
#include <limits.h>

#include "os-features.h"
#include "onefield.h"
#include "tweak.h"
#include "tweak2.h"
#include "sip_qfits.h"
#include "starutil.h"
#include "mathutil.h"
#include "quadfile.h"
#include "solvedfile.h"
#include "starkd.h"
#include "codekd.h"
#include "boilerplate.h"
#include "fitsioutils.h"
#include "verify.h"
#include "index.h"
#include "log.h"
#include "tic.h"
#include "anqfits.h"
#include "errors.h"
#include "scamp-catalog.h"
#include "permutedsort.h"
#include "bl-sort.h"
#include "keywords.h"

typedef enum index_shard_mode {
    INDEX_SHARD_OFF = 0,
    INDEX_SHARD_SERIAL_COMPAT = 1,
    INDEX_SHARD_SERIAL_REDUCE = 2,
    INDEX_SHARD_THREADS = 3
} index_shard_mode_t;

typedef struct onefield_index_shard {
    int shard_id;
    int index_start;
    int index_end;
    index_t* index;
    double load_ms;
} onefield_index_shard_t;

typedef struct onefield_shard_result {
    bl* solutions;
    int nsolves_sofar;
    anbool solved;
    anbool have_best_match;
    MatchObj best_match;
    double best_logodds;
    index_t* best_index;
    int status;
} onefield_shard_result_t;

typedef struct onefield_index_metric {
    int fieldnum;
    int shard_id;
    size_t index_order;
    const char* index_name;
    int index_id;
    int healpix;
    double scale_lower;
    double scale_upper;
    double load_ms;
    double solve_ms;
    double verify_ms;
    int numtries;
    int nummatches;
    int num_verified;
    double best_logodds;
    anbool solved;
    const char* exit_reason;
} onefield_index_metric_t;

typedef struct onefield_solve_metric_context {
    int shard_id;
    size_t index_order;
    index_t* index;
    double load_ms;
    anbool solved_before;
} onefield_solve_metric_context_t;

typedef struct onefield_shard_context onefield_shard_context_t;
typedef struct onefield_thread_pool onefield_thread_pool_t;

static anbool record_match_callback(MatchObj* mo, void* userdata);
static anbool record_match_shard_callback(MatchObj* mo, void* userdata);
static time_t timer_callback(void* user_data);
static time_t timer_shard_callback(void* user_data);
static void add_onefield_params(onefield_t* bp, qfits_header* hdr);
static void load_and_parse_wcsfiles(onefield_t* bp);
static void solve_fields(onefield_t* bp, sip_t* verify_wcs);
static void solve_fields_with_solver(onefield_t* bp, solver_t* sp,
                                     sip_t* verify_wcs,
                                     void* callback_userdata);
static void solve_fields_with_solver_and_metrics(
    onefield_t* bp, solver_t* sp, sip_t* verify_wcs,
    void* callback_userdata, onefield_solve_metric_context_t* metric_ctx);
static int make_one_index_shards(onefield_t* bp,
                                 onefield_index_shard_t** out_shards);
static int prepare_one_index_shards(onefield_t* bp,
                                    onefield_index_shard_t* shards,
                                    int Nshards);
static void release_one_index_shards(onefield_t* bp,
                                     onefield_index_shard_t* shards,
                                     int Nshards);
static int solver_init_shard_copy(solver_t* dst, const solver_t* src);
static int onefield_shard_context_init(onefield_shard_context_t* ctx,
                                       onefield_t* bp, solver_t* solver,
                                       const onefield_index_shard_t* shard);
static void onefield_shard_context_free(onefield_shard_context_t* ctx);
static void onefield_shard_result_capture(onefield_shard_context_t* ctx);
static void onefield_merge_shard_result(onefield_shard_context_t* ctx);
static int onefield_run_index_shards_serial_compat(onefield_t* bp);
static int onefield_run_index_shards_serial_reduce(onefield_t* bp);
static int onefield_run_index_shards_threaded_reduce(onefield_t* bp,
                                                     int workers);
static int onefield_open_worker_xylist(onefield_t* bp);
static void onefield_thread_pool_set_status(onefield_thread_pool_t* pool,
                                            int status);
static anbool onefield_index_loop_should_stop(onefield_t* bp);
static void run_index_candidate(onefield_t* bp, solver_t* sp, size_t I,
                                anbool trace_index_shards, int shard_id);
static void remove_invalid_fields(il* fieldlist, int maxfield);
static anbool is_field_solved(onefield_t* bp, int fieldnum);
static int write_solutions(onefield_t* bp);
static void solved_field(onefield_t* bp, int fieldnum);
static void check_time_limits_for_solver(onefield_t* bp, solver_t* sp);
static void matchobj_deep_copy_all(const MatchObj* mo, MatchObj* dest);
static void onefield_write_index_metric(const onefield_index_metric_t* metric);
static void onefield_write_index_metric_for_solver(onefield_t* bp,
                                                   solver_t* sp,
                                                   int shard_id,
                                                   size_t index_order,
                                                   index_t* index,
                                                   double load_ms,
                                                   double solve_ms,
                                                   anbool solved,
                                                   const char* exit_reason);
static const char* onefield_index_exit_reason(onefield_t* bp, solver_t* sp);
static void onefield_mark_reduced_solutions(onefield_t* bp);
static void* onefield_index_shard_worker(void* arg);
static int compare_matchobjs(const void* v1, const void* v2);
static void remove_duplicate_solutions(onefield_t* bp);

static anbool index_shard_trace_enabled(void) {
    const char* env = getenv("ASTROMETRY_INDEX_SHARD_TRACE");
    if (!env || !env[0])
        return FALSE;
    if (!strcmp(env, "0") || strcaseeq(env, "false") ||
        strcaseeq(env, "no") || strcaseeq(env, "off"))
        return FALSE;
    return TRUE;
}

static const char* index_shard_string_or_null(const char* str) {
    return str ? str : "(null)";
}

static const char* index_shard_mode_name(index_shard_mode_t mode) {
    switch (mode) {
    case INDEX_SHARD_OFF:
        return "off";
    case INDEX_SHARD_SERIAL_COMPAT:
        return "serial-compat";
    case INDEX_SHARD_SERIAL_REDUCE:
        return "serial-reduce";
    case INDEX_SHARD_THREADS:
        return "threads";
    }
    return "unknown";
}

static index_shard_mode_t index_shard_mode_from_env(void) {
    const char* env = getenv("ASTROMETRY_INDEX_SHARDS");
    if (!env || !env[0])
        return INDEX_SHARD_OFF;
    if (!strcmp(env, "0") || strcaseeq(env, "false") ||
        strcaseeq(env, "no") || strcaseeq(env, "off"))
        return INDEX_SHARD_OFF;
    if (strcaseeq(env, "serial-compat"))
        return INDEX_SHARD_SERIAL_COMPAT;
    if (strcaseeq(env, "serial-reduce"))
        return INDEX_SHARD_SERIAL_REDUCE;
    if (strcaseeq(env, "threads"))
        return INDEX_SHARD_THREADS;
    logerr("Unsupported ASTROMETRY_INDEX_SHARDS value \"%s\"; using off.\n", env);
    return INDEX_SHARD_OFF;
}

static index_shard_mode_t get_index_shard_mode(onefield_t* bp) {
    (void)bp;
    return index_shard_mode_from_env();
}

static const char* index_shard_metrics_path(void) {
    const char* env = getenv("ASTROMETRY_INDEX_SHARD_METRICS");
    if (!env || !env[0])
        return NULL;
    if (!strcmp(env, "0") || strcaseeq(env, "false") ||
        strcaseeq(env, "no") || strcaseeq(env, "off"))
        return NULL;
    return env;
}

static anbool index_shard_metrics_enabled(void) {
    return index_shard_metrics_path() != NULL;
}

static int index_shard_workers_from_env(void) {
    const char* env = getenv("ASTROMETRY_INDEX_SHARD_WORKERS");
    char* end = NULL;
    long workers;

    if (!env || !env[0])
        return 2;
    errno = 0;
    workers = strtol(env, &end, 10);
    if (errno || (end == env) || (workers < 1)) {
        logerr("Unsupported ASTROMETRY_INDEX_SHARD_WORKERS value \"%s\"; "
               "using 1.\n", env);
        return 1;
    }
    if (workers > INT_MAX)
        return INT_MAX;
    return (int)workers;
}

static const char* index_shard_metrics_run_id(void) {
    static char run_id[64];
    if (!run_id[0])
        snprintf(run_id, sizeof(run_id), "%ld-%ld",
                 (long)time(NULL), (long)getpid());
    return run_id;
}

static int index_shard_metrics_lock(int fd, short type) {
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = type;
    lock.l_whence = SEEK_SET;
    while (fcntl(fd, F_SETLKW, &lock) == -1) {
        if (errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static pthread_mutex_t index_shard_metrics_mutex = PTHREAD_MUTEX_INITIALIZER;

static void index_shard_metrics_csv_string(FILE* fid, const char* str) {
    const char* p;
    fputc('"', fid);
    if (str) {
        for (p=str; *p; p++) {
            if (*p == '"')
                fputc('"', fid);
            if ((*p == '\n') || (*p == '\r'))
                fputc(' ', fid);
            else
                fputc(*p, fid);
        }
    }
    fputc('"', fid);
}

static void onefield_write_index_metric(const onefield_index_metric_t* metric) {
    static anbool warned = FALSE;
    const char* path = index_shard_metrics_path();
    struct stat st;
    FILE* fid;
    int fd;
    anbool need_header = FALSE;

    if (!path)
        return;

    pthread_mutex_lock(&index_shard_metrics_mutex);
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd == -1) {
        if (!warned) {
            SYSERROR("Failed to open index shard metrics file");
            warned = TRUE;
        }
        pthread_mutex_unlock(&index_shard_metrics_mutex);
        return;
    }
    if (index_shard_metrics_lock(fd, F_WRLCK)) {
        if (!warned) {
            SYSERROR("Failed to lock index shard metrics file");
            warned = TRUE;
        }
        close(fd);
        pthread_mutex_unlock(&index_shard_metrics_mutex);
        return;
    }
    if (fstat(fd, &st) == 0)
        need_header = (st.st_size == 0);

    fid = fdopen(fd, "a");
    if (!fid) {
        if (!warned) {
            SYSERROR("Failed to fdopen index shard metrics file");
            warned = TRUE;
        }
        index_shard_metrics_lock(fd, F_UNLCK);
        close(fd);
        pthread_mutex_unlock(&index_shard_metrics_mutex);
        return;
    }

    if (need_header) {
        fprintf(fid, "run_id,fieldnum,shard_id,index_order,index_name,"
                "index_id,healpix,scale_lower,scale_upper,load_ms,solve_ms,"
                "verify_ms,numtries,nummatches,num_verified,best_logodds,"
                "solved,exit_reason\n");
    }

    index_shard_metrics_csv_string(fid, index_shard_metrics_run_id());
    fprintf(fid, ",%i,%i,%zu,", metric->fieldnum, metric->shard_id,
            metric->index_order);
    index_shard_metrics_csv_string(fid, metric->index_name);
    fprintf(fid, ",%i,%i,%.17g,%.17g,%.3f,%.3f,%.3f,%i,%i,%i,%.17g,%i,",
            metric->index_id, metric->healpix,
            metric->scale_lower, metric->scale_upper,
            metric->load_ms, metric->solve_ms, metric->verify_ms,
            metric->numtries, metric->nummatches, metric->num_verified,
            metric->best_logodds, metric->solved ? 1 : 0);
    index_shard_metrics_csv_string(fid, metric->exit_reason);
    fprintf(fid, "\n");

    fflush(fid);
    index_shard_metrics_lock(fd, F_UNLCK);
    fclose(fid);
    pthread_mutex_unlock(&index_shard_metrics_mutex);
}

static const char* onefield_index_exit_reason(onefield_t* bp, solver_t* sp) {
    if (bp->cancelled)
        return "cancelled";
    if (bp->hit_total_timelimit)
        return "total_timelimit";
    if (bp->hit_total_cpulimit)
        return "total_cpulimit";
    if (bp->hit_timelimit)
        return "timelimit";
    if (bp->hit_cpulimit)
        return "cpulimit";
    if (sp && sp->best_match_solves)
        return "solved";
    if (bp->single_field_solved)
        return "solved";
    if (sp && sp->maxquads && (sp->numtries >= sp->maxquads))
        return "maxquads";
    if (sp && sp->maxmatches && (sp->nummatches >= sp->maxmatches))
        return "maxmatches";
    if (sp && sp->quit_now)
        return "quit";
    return "exhausted";
}

static void onefield_write_index_metric_for_solver(onefield_t* bp,
                                                   solver_t* sp,
                                                   int shard_id,
                                                   size_t index_order,
                                                   index_t* index,
                                                   double load_ms,
                                                   double solve_ms,
                                                   anbool solved,
                                                   const char* exit_reason) {
    onefield_index_metric_t metric;

    if (!index_shard_metrics_enabled())
        return;

    memset(&metric, 0, sizeof(metric));
    metric.fieldnum = bp->fieldnum;
    metric.shard_id = shard_id;
    metric.index_order = index_order;
    metric.index_name = index && index->indexname ? index->indexname : "(null)";
    metric.index_id = index ? index->indexid : -1;
    metric.healpix = index ? index->healpix : -1;
    metric.scale_lower = index ? index->index_scale_lower : 0.0;
    metric.scale_upper = index ? index->index_scale_upper : 0.0;
    metric.load_ms = load_ms;
    metric.solve_ms = solve_ms;
    metric.verify_ms = sp ? sp->verify_timeused * 1000.0 : 0.0;
    metric.numtries = sp ? sp->numtries : 0;
    metric.nummatches = sp ? sp->nummatches : 0;
    metric.num_verified = sp ? sp->num_verified : 0;
    metric.best_logodds = sp ? sp->best_logodds : 0.0;
    metric.solved = solved;
    metric.exit_reason = exit_reason ? exit_reason : "unknown";

    onefield_write_index_metric(&metric);
}

// A tag-along column for index rdls / correspondence file.
struct tagalong {
    tfits_type type;
    int arraysize;
    char* name;
    char* units;
    void* data;
    // size in bytes of one item.
    int itemsize;
    int Ndata;
    // assigned by rdlist_add_tagalong_column
    int colnum;
};
typedef struct tagalong tagalong_t;

typedef struct record_match_context {
    onefield_t* bp;
    solver_t* sp;
    bl* solutions;
    int* nsolves_sofar;
} record_match_context_t;

struct onefield_shard_context {
    onefield_t* bp;
    solver_t* solver;
    onefield_index_shard_t shard;
    onefield_shard_result_t result;
    record_match_context_t callback_ctx;
};

struct onefield_thread_pool {
    onefield_t* bp;
    onefield_index_shard_t* shards;
    onefield_shard_context_t* contexts;
    int Nshards;
    int next_shard;
    anbool metrics;
    anbool trace;
    int status;
    pthread_mutex_t lock;
};

static anbool grab_tagalong_data(startree_t* starkd, MatchObj* mo, onefield_t* bp,
                                 const int* starinds, int N) {
    fitstable_t* tagalong;
    int i;
    tagalong = startree_get_tagalong(starkd);
    if (!tagalong) {
        ERROR("Failed to find tag-along table in index");
        return FALSE;
    }
    if (!mo->tagalong)
        mo->tagalong = bl_new(16, sizeof(tagalong_t));

    if (bp->rdls_tagalong_all) { // && ! bp->done_rdls_tagalong_all
        char* cols;
        // retrieve all column names.
        bp->rdls_tagalong = fitstable_get_fits_column_names(tagalong, bp->rdls_tagalong);
        cols = sl_join(bp->rdls_tagalong, ", ");
        logverb("Found tag-along columns: %s\n", cols);
        free(cols);
        //
        sl_remove_duplicates(bp->rdls_tagalong);
        cols = sl_join(bp->rdls_tagalong, ", ");
        logverb("After removing duplicates: %s\n", cols);
        free(cols);
    }
    for (i=0; i<sl_size(bp->rdls_tagalong); i++) {
        const char* col = sl_get(bp->rdls_tagalong, i);
        tagalong_t tag;
        if (fitstable_find_fits_column(tagalong, col, &(tag.units), &(tag.type), &(tag.arraysize))) {
            ERROR("Failed to find column \"%s\" in index", col);
            continue;
        }
        tag.data = fitstable_read_column_array_inds(tagalong, col, tag.type, starinds, N, NULL);
        if (!tag.data) {
            ERROR("Failed to read data for column \"%s\" in index", col);
            continue;
        }
        if ((strcaseeq(col, "ra") || strcaseeq(col, "dec")))
            asprintf_safe(&(tag.name), "%s_ref", col);
        else
            tag.name = strdup(col);
        tag.units = strdup(tag.units);
        tag.itemsize = fits_get_atom_size(tag.type) * tag.arraysize;
        tag.Ndata = N;
        bl_append(mo->tagalong, &tag);
    }
    return TRUE;
}

static anbool grab_field_tagalong_data(MatchObj* mo, xylist_t* xy, int N) {
    fitstable_t* tagalong;
    int i;
    sl* lst;
    if (!mo->field_tagalong)
        mo->field_tagalong = bl_new(16, sizeof(tagalong_t));
    tagalong = xy->table;
    lst = xylist_get_tagalong_column_names(xy, NULL);
    {
        char* txt = sl_join(lst, " ");
        logverb("Found tag-along columns from field: %s\n", txt);
        free(txt);
    }
    for (i=0; i<sl_size(lst); i++) {
        const char* col = sl_get(lst, i);
        tagalong_t tag;
        if (fitstable_find_fits_column(tagalong, col, &(tag.units), &(tag.type), &(tag.arraysize))) {
            ERROR("Failed to find column \"%s\" in index", col);
            continue;
        }
        tag.data = fitstable_read_column_array(tagalong, col, tag.type);
        if (!tag.data) {
            ERROR("Failed to read data for column \"%s\" in index", col);
            continue;
        }
        tag.name = strdup(col);
        tag.units = strdup(tag.units);
        tag.itemsize = fits_get_atom_size(tag.type) * tag.arraysize;
        tag.Ndata = N;
        bl_append(mo->field_tagalong, &tag);
    }
    sl_free2(lst);
    return TRUE;
}


/** Index handling for in_parallel and not.

 Currently it supposedly could handle both "indexnames" and "indexes",
 but we should probably just assert that only one of these can be used.
 **/
static index_t* get_index(onefield_t* bp, size_t i) {
    if (i < sl_size(bp->indexnames)) {
        char* fn = sl_get(bp->indexnames, i);
        index_t* ind = index_load(fn, bp->index_options, NULL);
        if (!ind) {
            ERROR("Failed to load index %s", fn);
            exit( -1);
        }
        return ind;
    }
    i -= sl_size(bp->indexnames);
    return pl_get(bp->indexes, i);
}
static char* get_index_name(onefield_t* bp, size_t i) {
    index_t* index;
    if (i < sl_size(bp->indexnames)) {
        char* fn = sl_get(bp->indexnames, i);
        return fn;
    }
    i -= sl_size(bp->indexnames);
    index = pl_get(bp->indexes, i);
    return index->indexname;
}
static void done_with_index(onefield_t* bp, size_t i, index_t* ind) {
    if (i < sl_size(bp->indexnames)) {
        index_close(ind);
    }
}
static size_t n_indexes(onefield_t* bp) {
    return sl_size(bp->indexnames) + pl_size(bp->indexes);
}

static int make_one_index_shards(onefield_t* bp,
                                 onefield_index_shard_t** out_shards) {
    onefield_index_shard_t* shards;
    size_t Nindexes = n_indexes(bp);
    int i;

    *out_shards = NULL;
    if (Nindexes > INT_MAX) {
        ERROR("Too many candidate indexes for one-index shards: %zu", Nindexes);
        return -1;
    }
    if (!Nindexes)
        return 0;

    shards = calloc(Nindexes, sizeof(onefield_index_shard_t));
    if (!shards) {
        SYSERROR("Failed to allocate index shards");
        return -1;
    }

    for (i=0; i<(int)Nindexes; i++) {
        shards[i].shard_id = i;
        shards[i].index_start = i;
        shards[i].index_end = i + 1;
    }

    *out_shards = shards;
    return (int)Nindexes;
}

static int prepare_one_index_shards(onefield_t* bp,
                                    onefield_index_shard_t* shards,
                                    int Nshards) {
    int s;
    for (s=0; s<Nshards; s++) {
        onefield_index_shard_t* shard = shards + s;
        double load_t0 = timenow();
        if (shard->index_start < 0 || shard->index_start >= (int)n_indexes(bp)) {
            ERROR("Invalid shard index range [%i,%i)",
                  shard->index_start, shard->index_end);
            return -1;
        }
        shard->index = get_index(bp, shard->index_start);
        shard->load_ms = 1000.0 * (timenow() - load_t0);
    }
    return 0;
}

static void release_one_index_shards(onefield_t* bp,
                                     onefield_index_shard_t* shards,
                                     int Nshards) {
    int s;
    for (s=0; s<Nshards; s++) {
        onefield_index_shard_t* shard = shards + s;
        if (!shard->index)
            continue;
        done_with_index(bp, shard->index_start, shard->index);
        shard->index = NULL;
    }
}

static int solver_init_shard_copy(solver_t* dst, const solver_t* src) {
    solver_set_default_values(dst);

    dst->pixel_xscale = src->pixel_xscale;
    if (src->predistort) {
        dst->predistort = sip_create();
        if (!dst->predistort) {
            SYSERROR("Failed to allocate shard predistortion");
            solver_cleanup(dst);
            return -1;
        }
        sip_copy(dst->predistort, src->predistort);
    }

    dst->funits_lower = src->funits_lower;
    dst->funits_upper = src->funits_upper;
    dst->logratio_toprint = src->logratio_toprint;
    dst->logratio_tokeep = src->logratio_tokeep;
    dst->logratio_totune = src->logratio_totune;
    dst->distance_from_quad_bonus = src->distance_from_quad_bonus;
    dst->verify_uniformize = src->verify_uniformize;
    dst->verify_dedup = src->verify_dedup;
    dst->do_tweak = src->do_tweak;
    dst->tweak_aborder = src->tweak_aborder;
    dst->tweak_abporder = src->tweak_abporder;

    dst->verify_pix = src->verify_pix;
    dst->distractor_ratio = src->distractor_ratio;
    dst->codetol = src->codetol;
    dst->quadsize_min = src->quadsize_min;
    dst->quadsize_max = src->quadsize_max;
    dst->startobj = src->startobj;
    dst->endobj = src->endobj;
    dst->parity = src->parity;
    dst->use_radec = src->use_radec;
    memcpy(dst->centerxyz, src->centerxyz, sizeof(dst->centerxyz));
    dst->r2 = src->r2;
    dst->logratio_bail_threshold = src->logratio_bail_threshold;
    dst->logratio_stoplooking = src->logratio_stoplooking;
    dst->maxquads = src->maxquads;
    dst->maxmatches = src->maxmatches;
    dst->set_crpix = src->set_crpix;
    dst->set_crpix_center = src->set_crpix_center;
    memcpy(dst->crpix, src->crpix, sizeof(dst->crpix));

    dst->field_minx = src->field_minx;
    dst->field_maxx = src->field_maxx;
    dst->field_miny = src->field_miny;
    dst->field_maxy = src->field_maxy;
    dst->field_diag = src->field_diag;

    return 0;
}

static void onefield_free_matchobj_list(bl* solutions) {
    int i;
    for (i=0; i<bl_size(solutions); i++) {
        MatchObj* mo = bl_access(solutions, i);
        verify_free_matchobj(mo);
        onefield_free_matchobj(mo);
    }
    bl_remove_all(solutions);
}

static int onefield_shard_context_init(onefield_shard_context_t* ctx,
                                       onefield_t* bp, solver_t* solver,
                                       const onefield_index_shard_t* shard) {
    memset(ctx, 0, sizeof(onefield_shard_context_t));
    ctx->bp = bp;
    ctx->solver = solver;
    ctx->shard = *shard;
    ctx->result.solutions = bl_new(16, sizeof(MatchObj));
    if (!ctx->result.solutions)
        return -1;
    ctx->callback_ctx.bp = bp;
    ctx->callback_ctx.sp = solver;
    ctx->callback_ctx.solutions = ctx->result.solutions;
    ctx->callback_ctx.nsolves_sofar = &(ctx->result.nsolves_sofar);
    return 0;
}

static void onefield_shard_result_capture(onefield_shard_context_t* ctx) {
    solver_t* sp = ctx->solver;
    onefield_shard_result_t* result = &(ctx->result);
    onefield_t* bp = ctx->bp;
    double best_logodds = 0.0;
    int max_solved_count = 0;
    anbool solved = FALSE;
    int best = -1;
    int i;

    if (result->have_best_match) {
        verify_free_matchobj(&(result->best_match));
        onefield_free_matchobj(&(result->best_match));
        result->have_best_match = FALSE;
    }

    result->status = 0;

    /*
     * solve_fields_with_solver_and_metrics() cleans up field-local solver
     * state before returning, so the shard's durable result is the local
     * MatchObj list populated by the callback.
     */
    for (i=0; i<bl_size(result->solutions); i++) {
        MatchObj* mo = bl_access(result->solutions, i);
        if ((best < 0) || (mo->logodds > best_logodds)) {
            best = i;
            best_logodds = mo->logodds;
        }
        if (mo->logodds >= bp->logratio_tosolve) {
            int field_solved_count = 0;
            int j;
            for (j=0; j<bl_size(result->solutions); j++) {
                MatchObj* candidate = bl_access(result->solutions, j);
                if (candidate->fieldnum != mo->fieldnum)
                    continue;
                if (candidate->logodds >= bp->logratio_tosolve)
                    field_solved_count++;
            }
            if (field_solved_count > max_solved_count)
                max_solved_count = field_solved_count;
            if (field_solved_count >= bp->nsolves)
                solved = TRUE;
        }
    }

    result->nsolves_sofar = max_solved_count;
    result->solved = solved;
    result->best_logodds = best_logodds;
    result->best_index = sp->best_index;
    if (best >= 0) {
        MatchObj* mo = bl_access(result->solutions, best);
        matchobj_deep_copy_all(mo, &(result->best_match));
        result->have_best_match = TRUE;
    } else if (sp->have_best_match) {
        result->best_logodds = sp->best_logodds;
        result->best_index = sp->best_index;
        matchobj_deep_copy_all(&(sp->best_match), &(result->best_match));
        result->have_best_match = TRUE;
    } else {
        result->best_logodds = sp->best_logodds;
    }
}

static void onefield_merge_shard_result(onefield_shard_context_t* ctx) {
    onefield_t* bp = ctx->bp;
    bl* local_solutions = ctx->result.solutions;
    int i;

    for (i=0; i<bl_size(local_solutions); i++) {
        MatchObj* mo = bl_access(local_solutions, i);
        MatchObj copy;
        matchobj_deep_copy_all(mo, &copy);
        bl_insert_sorted(bp->solutions, &copy, compare_matchobjs);
    }
}

static void onefield_shard_context_free(onefield_shard_context_t* ctx) {
    if (ctx->result.solutions) {
        onefield_free_matchobj_list(ctx->result.solutions);
        bl_free(ctx->result.solutions);
        ctx->result.solutions = NULL;
    }
    if (ctx->result.have_best_match) {
        verify_free_matchobj(&(ctx->result.best_match));
        onefield_free_matchobj(&(ctx->result.best_match));
        ctx->result.have_best_match = FALSE;
    }
}

static void onefield_mark_reduced_solutions(onefield_t* bp) {
    il* solved_fields;
    int i, j;

    if (!bl_size(bp->solutions))
        return;

    solved_fields = il_new(16);
    for (i=0; i<bl_size(bp->solutions); i++) {
        MatchObj* mo = bl_access(bp->solutions, i);
        int fieldnum = mo->fieldnum;
        int nsolves = 0;

        if (il_contains(solved_fields, fieldnum))
            continue;
        if (mo->logodds < bp->logratio_tosolve)
            continue;

        for (j=0; j<bl_size(bp->solutions); j++) {
            MatchObj* candidate = bl_access(bp->solutions, j);
            if (candidate->fieldnum != fieldnum)
                continue;
            if (candidate->logodds >= bp->logratio_tosolve)
                nsolves++;
            if (nsolves >= bp->nsolves)
                break;
        }
        if (nsolves >= bp->nsolves) {
            solved_field(bp, fieldnum);
            il_insert_unique_ascending(solved_fields, fieldnum);
        }
    }
    il_free(solved_fields);
}

static anbool onefield_index_loop_should_stop(onefield_t* bp) {
    if (bp->hit_total_timelimit || bp->hit_total_cpulimit)
        return TRUE;
    if (bp->single_field_solved)
        return TRUE;
    if (bp->cancelled)
        return TRUE;
    return FALSE;
}

static void run_index_candidate(onefield_t* bp, solver_t* sp, size_t I,
                                anbool trace_index_shards, int shard_id) {
    index_t* index;
    anbool metrics = index_shard_metrics_enabled();
    onefield_solve_metric_context_t metric_ctx;
    onefield_solve_metric_context_t* metricp = NULL;
    double load_t0 = 0.0;
    double load_ms = 0.0;

    if (metrics)
        load_t0 = timenow();
    index = get_index(bp, I);
    if (metrics)
        load_ms = 1000.0 * (timenow() - load_t0);
    solver_add_index(sp, index);
    if (trace_index_shards) {
        if (shard_id >= 0)
            logmsg("[index-shard] processing shard=%i candidate_order=%zu index=%s\n",
                   shard_id, I, index_shard_string_or_null(index->indexname));
        else
            logmsg("[index-shard] processing order=%zu index=%s\n",
                   I, index_shard_string_or_null(index->indexname));
    }
    logverb("Trying index %s...\n", index->indexname);

    // Record current CPU usage.
    bp->cpu_start = get_cpu_usage();
    // Record current wall-clock time.
    bp->time_start = time(NULL);

    // Do it!
    if (metrics) {
        memset(&metric_ctx, 0, sizeof(metric_ctx));
        metric_ctx.shard_id = shard_id;
        metric_ctx.index_order = I;
        metric_ctx.index = index;
        metric_ctx.load_ms = load_ms;
        metric_ctx.solved_before = bp->single_field_solved;
        metricp = &metric_ctx;
    }
    solve_fields_with_solver_and_metrics(bp, sp, NULL, bp, metricp);

    // Clean up this index...
    done_with_index(bp, I, index);
    solver_clear_indexes(sp);
}

static int onefield_run_index_shards_serial_compat(onefield_t* bp) {
    onefield_index_shard_t* shards = NULL;
    size_t Nindexes = n_indexes(bp);
    int Nshards = make_one_index_shards(bp, &shards);
    int s;
    anbool trace_index_shards =
        index_shard_trace_enabled() ||
        (get_index_shard_mode(bp) == INDEX_SHARD_SERIAL_COMPAT);
    anbool metrics = index_shard_metrics_enabled();

    if (Nshards < 0)
        return -1;

    if (trace_index_shards) {
        logmsg("[index-shard] serial-compat shards=%i candidates=%zu "
               "granularity=one-index\n", Nshards, Nindexes);
        for (s=0; s<Nshards; s++) {
            onefield_index_shard_t* shard = shards + s;
            logmsg("[index-shard] shard=%i range=[%i,%i) "
                   "candidate_order=%i index=%s\n",
                   shard->shard_id, shard->index_start, shard->index_end,
                   shard->index_start,
                   index_shard_string_or_null(
                       get_index_name(bp, shard->index_start)));
        }
    }

    for (s=0; s<Nshards; s++) {
        onefield_index_shard_t* shard = shards + s;
        solver_t local_solver;
        onefield_shard_context_t shard_ctx;
        anbool solved_before;
        size_t solutions_before;
        anbool stop;
        size_t I;

        if (onefield_index_loop_should_stop(bp))
            break;
        if (solver_init_shard_copy(&local_solver, &(bp->solver))) {
            free(shards);
            return -1;
        }
        if (onefield_shard_context_init(&shard_ctx, bp, &local_solver, shard)) {
            solver_cleanup(&local_solver);
            free(shards);
            return -1;
        }

        solved_before = bp->single_field_solved;
        solutions_before = bl_size(bp->solutions);

        if (trace_index_shards) {
            logmsg("[index-shard] shard=%i start range=[%i,%i)\n",
                   shard->shard_id, shard->index_start, shard->index_end);
        }

        for (I=(size_t)shard->index_start; I<(size_t)shard->index_end; I++) {
            index_t* index;
            onefield_solve_metric_context_t metric_ctx;
            onefield_solve_metric_context_t* metricp = NULL;
            double load_t0 = 0.0;
            double load_ms = 0.0;

            if (onefield_index_loop_should_stop(bp))
                break;

            if (metrics)
                load_t0 = timenow();
            index = get_index(bp, I);
            if (metrics)
                load_ms = 1000.0 * (timenow() - load_t0);
            solver_add_index(&local_solver, index);
            if (trace_index_shards)
                logmsg("[index-shard] processing shard=%i "
                       "candidate_order=%zu index=%s\n",
                       shard->shard_id, I,
                       index_shard_string_or_null(index->indexname));
            logverb("Trying index %s...\n", index->indexname);

            // Record current CPU usage.
            bp->cpu_start = get_cpu_usage();
            // Record current wall-clock time.
            bp->time_start = time(NULL);

            if (metrics) {
                memset(&metric_ctx, 0, sizeof(metric_ctx));
                metric_ctx.shard_id = shard->shard_id;
                metric_ctx.index_order = I;
                metric_ctx.index = index;
                metric_ctx.load_ms = load_ms;
                metric_ctx.solved_before = solved_before;
                metricp = &metric_ctx;
            }
            solve_fields_with_solver_and_metrics(
                bp, &local_solver, NULL, &(shard_ctx.callback_ctx), metricp);

            done_with_index(bp, I, index);
            solver_clear_indexes(&local_solver);
        }

        onefield_shard_result_capture(&shard_ctx);
        onefield_merge_shard_result(&shard_ctx);
        stop = onefield_index_loop_should_stop(bp);
        if (trace_index_shards) {
            logmsg("[index-shard] shard=%i end solved=%i "
                   "solutions_delta=%zu stop=%i\n",
                   shard->shard_id,
                   shard_ctx.result.solved ||
                   (!solved_before && bp->single_field_solved) ? 1 : 0,
                   bl_size(bp->solutions) - solutions_before,
                   stop ? 1 : 0);
        }
        onefield_shard_context_free(&shard_ctx);
        solver_cleanup(&local_solver);
        if (stop)
            break;
    }

    free(shards);
    return 0;
}

static int onefield_open_worker_xylist(onefield_t* bp) {
    bp->xyls = xylist_open(bp->fieldfname);
    if (!bp->xyls) {
        ERROR("Failed to read worker xylist");
        return -1;
    }
    xylist_set_xname(bp->xyls, bp->xcolname);
    xylist_set_yname(bp->xyls, bp->ycolname);
    xylist_set_include_flux(bp->xyls, FALSE);
    xylist_set_include_background(bp->xyls, FALSE);
    return 0;
}

static void onefield_thread_pool_set_status(onefield_thread_pool_t* pool,
                                            int status) {
    pthread_mutex_lock(&(pool->lock));
    if (status && !pool->status)
        pool->status = status;
    pthread_mutex_unlock(&(pool->lock));
}

static void onefield_thread_pool_merge_flags(onefield_thread_pool_t* pool,
                                             onefield_t* local_bp) {
    pthread_mutex_lock(&(pool->lock));
    pool->bp->cancelled |= local_bp->cancelled;
    pool->bp->hit_cpulimit |= local_bp->hit_cpulimit;
    pool->bp->hit_timelimit |= local_bp->hit_timelimit;
    pool->bp->hit_total_cpulimit |= local_bp->hit_total_cpulimit;
    pool->bp->hit_total_timelimit |= local_bp->hit_total_timelimit;
    pthread_mutex_unlock(&(pool->lock));
}

static void* onefield_index_shard_worker(void* arg) {
    onefield_thread_pool_t* pool = arg;

    while (TRUE) {
        int s;
        onefield_index_shard_t* shard;
        onefield_shard_context_t* ctx;
        solver_t local_solver;
        onefield_t local_bp;
        onefield_solve_metric_context_t metric_ctx;
        onefield_solve_metric_context_t* metricp = NULL;
        anbool context_initialized = FALSE;

        pthread_mutex_lock(&(pool->lock));
        s = pool->next_shard++;
        pthread_mutex_unlock(&(pool->lock));

        if (s >= pool->Nshards)
            break;

        shard = pool->shards + s;
        ctx = pool->contexts + s;
        local_bp = *(pool->bp);
        local_bp.xyls = NULL;
        local_bp.solved_out = NULL;
        local_bp.single_field_solved = FALSE;
        local_bp.cancelled = FALSE;
        local_bp.hit_cpulimit = FALSE;
        local_bp.hit_timelimit = FALSE;
        local_bp.hit_total_cpulimit = FALSE;
        local_bp.hit_total_timelimit = FALSE;

        if (pool->trace) {
            logmsg("[index-shard] shard=%i thread-start range=[%i,%i) "
                   "candidate_order=%i index=%s\n",
                   shard->shard_id, shard->index_start, shard->index_end,
                   shard->index_start,
                   index_shard_string_or_null(
                       shard->index ? shard->index->indexname : NULL));
        }

        if (onefield_open_worker_xylist(&local_bp)) {
            ctx->result.status = -1;
            onefield_thread_pool_set_status(pool, -1);
            continue;
        }
        if (solver_init_shard_copy(&local_solver, &(pool->bp->solver))) {
            xylist_close(local_bp.xyls);
            ctx->result.status = -1;
            onefield_thread_pool_set_status(pool, -1);
            continue;
        }
        if (onefield_shard_context_init(ctx, pool->bp, &local_solver, shard)) {
            solver_cleanup(&local_solver);
            xylist_close(local_bp.xyls);
            ctx->result.status = -1;
            onefield_thread_pool_set_status(pool, -1);
            continue;
        }
        context_initialized = TRUE;

        ctx->callback_ctx.bp = &local_bp;
        ctx->callback_ctx.sp = &local_solver;
        ctx->solver = &local_solver;

        solver_add_index(&local_solver, shard->index);
        if (pool->trace) {
            logmsg("[index-shard] processing shard=%i candidate_order=%i "
                   "index=%s\n",
                   shard->shard_id, shard->index_start,
                   index_shard_string_or_null(
                       shard->index ? shard->index->indexname : NULL));
        }
        if (shard->index && shard->index->indexname)
            logverb("Trying index %s...\n", shard->index->indexname);

        local_bp.cpu_start = get_cpu_usage();
        local_bp.time_start = time(NULL);

        if (pool->metrics) {
            memset(&metric_ctx, 0, sizeof(metric_ctx));
            metric_ctx.shard_id = shard->shard_id;
            metric_ctx.index_order = shard->index_start;
            metric_ctx.index = shard->index;
            metric_ctx.load_ms = shard->load_ms;
            metric_ctx.solved_before = FALSE;
            metricp = &metric_ctx;
        }

        solve_fields_with_solver_and_metrics(
            &local_bp, &local_solver, NULL, &(ctx->callback_ctx), metricp);
        onefield_shard_result_capture(ctx);
        onefield_thread_pool_merge_flags(pool, &local_bp);

        if (pool->trace) {
            logmsg("[index-shard] shard=%i thread-end solved=%i "
                   "solutions=%zu status=%i\n",
                   shard->shard_id, ctx->result.solved ? 1 : 0,
                   bl_size(ctx->result.solutions), ctx->result.status);
        }

        solver_clear_indexes(&local_solver);
        solver_cleanup(&local_solver);
        xylist_close(local_bp.xyls);

        if (!context_initialized)
            ctx->result.status = -1;
    }

    return NULL;
}

static int onefield_run_index_shards_threaded_reduce(onefield_t* bp,
                                                     int workers) {
    onefield_index_shard_t* shards = NULL;
    onefield_shard_context_t* contexts = NULL;
    pthread_t* threads = NULL;
    onefield_thread_pool_t pool;
    int Nshards = make_one_index_shards(bp, &shards);
    int s;
    int started = 0;
    int status = 0;
    anbool trace_index_shards =
        index_shard_trace_enabled() ||
        (get_index_shard_mode(bp) != INDEX_SHARD_OFF);
    anbool metrics = index_shard_metrics_enabled();

    if (Nshards < 0)
        return -1;
    if (!Nshards)
        return 0;
    if (prepare_one_index_shards(bp, shards, Nshards)) {
        release_one_index_shards(bp, shards, Nshards);
        free(shards);
        return -1;
    }

    contexts = calloc(Nshards, sizeof(onefield_shard_context_t));
    if (!contexts) {
        SYSERROR("Failed to allocate index shard contexts");
        release_one_index_shards(bp, shards, Nshards);
        free(shards);
        return -1;
    }

    if (workers < 1)
        workers = 1;
    if (workers > Nshards)
        workers = Nshards;

    if (trace_index_shards) {
        logmsg("[index-shard] threaded-reduce shards=%i workers=%i "
               "granularity=one-index\n", Nshards, workers);
        for (s=0; s<Nshards; s++) {
            onefield_index_shard_t* shard = shards + s;
            logmsg("[index-shard] shard=%i range=[%i,%i) "
                   "candidate_order=%i index=%s load_ms=%.3f\n",
                   shard->shard_id, shard->index_start, shard->index_end,
                   shard->index_start,
                   index_shard_string_or_null(
                       shard->index ? shard->index->indexname : NULL),
                   shard->load_ms);
        }
    }

    memset(&pool, 0, sizeof(pool));
    pool.bp = bp;
    pool.shards = shards;
    pool.contexts = contexts;
    pool.Nshards = Nshards;
    pool.metrics = metrics;
    pool.trace = trace_index_shards;

    {
        int err = pthread_mutex_init(&(pool.lock), NULL);
        if (err) {
            errno = err;
            SYSERROR("Failed to initialize index shard thread lock");
            release_one_index_shards(bp, shards, Nshards);
            free(contexts);
            free(shards);
            return -1;
        }
    }

    if (workers > 1)
        threads = calloc(workers, sizeof(pthread_t));
    if ((workers > 1) && !threads) {
        SYSERROR("Failed to allocate index shard threads; falling back to one worker");
        workers = 1;
    }

    if (workers == 1) {
        onefield_index_shard_worker(&pool);
    } else {
        for (started=0; started<workers; started++) {
            int err = pthread_create(threads + started, NULL,
                                     onefield_index_shard_worker, &pool);
            if (err) {
                errno = err;
                SYSERROR("Failed to start index shard worker; "
                         "main thread will finish remaining shards");
                break;
            }
        }
        if (!started)
            onefield_index_shard_worker(&pool);
        else if (started < workers)
            onefield_index_shard_worker(&pool);

        for (s=0; s<started; s++) {
            int err = pthread_join(threads[s], NULL);
            if (err) {
                errno = err;
                SYSERROR("Failed to join index shard worker");
                status = -1;
            }
        }
    }

    if (pool.status)
        status = pool.status;

    if (!status) {
        for (s=0; s<Nshards; s++) {
            onefield_merge_shard_result(contexts + s);
            if (trace_index_shards) {
                logmsg("[index-shard] shard=%i reduce-merge solved=%i "
                       "solutions=%zu\n",
                       contexts[s].shard.shard_id,
                       contexts[s].result.solved ? 1 : 0,
                       bl_size(contexts[s].result.solutions));
            }
        }
        onefield_mark_reduced_solutions(bp);
    }

    for (s=0; s<Nshards; s++)
        onefield_shard_context_free(contexts + s);

    free(threads);
    pthread_mutex_destroy(&(pool.lock));
    release_one_index_shards(bp, shards, Nshards);
    free(contexts);
    free(shards);
    return status;
}

static int onefield_run_index_shards_serial_reduce(onefield_t* bp) {
    return onefield_run_index_shards_threaded_reduce(bp, 1);
}



void onefield_clear_verify_wcses(onefield_t* bp) {
    bl_remove_all(bp->verify_wcs_list);
}

void onefield_clear_solutions(onefield_t* bp) {
    bl_remove_all(bp->solutions);
}

void onefield_clear_indexes(onefield_t* bp) {
    sl_remove_all(bp->indexnames);
    pl_remove_all(bp->indexes);
}

void onefield_set_field_file(onefield_t* bp, const char* fn) {
    free(bp->fieldfname);
    bp->fieldfname = strdup_safe(fn);
}

void onefield_set_solved_file(onefield_t* bp, const char* fn) {
    onefield_set_solvedin_file (bp, fn);
    onefield_set_solvedout_file(bp, fn);
}

void onefield_set_solvedin_file(onefield_t* bp, const char* fn) {
    free(bp->solved_in);
    bp->solved_in = strdup_safe(fn);
}

void onefield_set_solvedout_file(onefield_t* bp, const char* fn) {
    free(bp->solved_out);
    bp->solved_out = strdup_safe(fn);
}

void onefield_set_cancel_file(onefield_t* bp, const char* fn) {
    free(bp->cancelfname);
    bp->cancelfname = strdup_safe(fn);
}

void onefield_set_match_file(onefield_t* bp, const char* fn) {
    free(bp->matchfname);
    bp->matchfname = strdup_safe(fn);
}

void onefield_set_rdls_file(onefield_t* bp, const char* fn) {
    free(bp->indexrdlsfname);
    bp->indexrdlsfname = strdup_safe(fn);
}

void onefield_set_scamp_file(onefield_t* bp, const char* fn) {
    free(bp->scamp_fname);
    bp->scamp_fname = strdup_safe(fn);
}

void onefield_set_corr_file(onefield_t* bp, const char* fn) {
    free(bp->corr_fname);
    bp->corr_fname = strdup_safe(fn);
}

void onefield_set_wcs_file(onefield_t* bp, const char* fn) {
    free(bp->wcs_template);
    bp->wcs_template = strdup_safe(fn);
}

void onefield_set_xcol(onefield_t* bp, const char* x) {
    free(bp->xcolname);
    if (!x)
        x = "X";
    bp->xcolname = strdup(x);
}

void onefield_set_ycol(onefield_t* bp, const char* y) {
    free(bp->ycolname);
    if (!y)
        y = "Y";
    bp->ycolname = strdup_safe(y);
}

void onefield_add_index(onefield_t* bp, const char* index) {
    sl_append(bp->indexnames, index);
}

void onefield_add_loaded_index(onefield_t* bp, index_t* ind) {
    pl_append(bp->indexes, ind);
}

void onefield_add_verify_wcs(onefield_t* bp, sip_t* wcs) {
    bl_append(bp->verify_wcs_list, wcs);
}

void onefield_add_field(onefield_t* bp, int field) {
    il_insert_unique_ascending(bp->fieldlist, field);
}

void onefield_add_field_range(onefield_t* bp, int lo, int hi) {
    int i;
    for (i=lo; i<=hi; i++) {
        il_insert_unique_ascending(bp->fieldlist, i);
    }
}

static void check_time_limits_for_solver(onefield_t* bp, solver_t* sp) {
    if (bp->total_timelimit || bp->timelimit) {
        double now = timenow();
        if (bp->total_timelimit && (now - bp->time_total_start > bp->total_timelimit)) {
            logmsg("Total wall-clock time limit reached!\n");
            bp->hit_total_timelimit = TRUE;
        }
        if (bp->timelimit && (now - bp->time_start > bp->timelimit)) {
            logmsg("Wall-clock time limit reached!\n");
            bp->hit_timelimit = TRUE;
        }
    }
    if (bp->total_cpulimit || bp->cpulimit) {
        float now = get_cpu_usage();
        if ((bp->total_cpulimit > 0.0) &&
            (now - bp->cpu_total_start > bp->total_cpulimit)) {
            logmsg("Total CPU time limit reached!\n");
            bp->hit_total_cpulimit = TRUE;
        }
        if ((bp->cpulimit > 0.0) &&
            (now - bp->cpu_start > bp->cpulimit)) {
            logmsg("CPU time limit reached!\n");
            bp->hit_cpulimit = TRUE;
        }
    }
    if (bp->hit_total_timelimit ||
        bp->hit_total_cpulimit ||
        bp->hit_timelimit ||
        bp->hit_cpulimit)
        sp->quit_now = TRUE;
}

static void check_time_limits(onefield_t* bp) {
    check_time_limits_for_solver(bp, &(bp->solver));
}

void onefield_run(onefield_t* bp) {
    solver_t* sp = &(bp->solver);
    size_t i, I;
    size_t Nindexes;
    index_shard_mode_t shard_mode = get_index_shard_mode(bp);
    anbool trace_index_shards =
        index_shard_trace_enabled() || (shard_mode != INDEX_SHARD_OFF);
    anbool use_serial_compat_shards;
    anbool use_serial_reduce_shards;
    anbool use_threaded_shards;

    // Record current time for total wall-clock time limit.
    bp->time_total_start = timenow();

    // Record current CPU usage for total cpu-usage limit.
    bp->cpu_total_start = get_cpu_usage();

    // Parse WCS files submitted for verification.
    load_and_parse_wcsfiles(bp);

    // Read .xyls file...
    logverb("Reading fields file %s...", bp->fieldfname);
    bp->xyls = xylist_open(bp->fieldfname);
    if (!bp->xyls) {
        ERROR("Failed to read xylist.\n");
        exit( -1);
    }
    xylist_set_xname(bp->xyls, bp->xcolname);
    xylist_set_yname(bp->xyls, bp->ycolname);
    xylist_set_include_flux(bp->xyls, FALSE);
    xylist_set_include_background(bp->xyls, FALSE);
    logverb("found %u fields.\n", xylist_n_fields(bp->xyls));

    remove_invalid_fields(bp->fieldlist, xylist_n_fields(bp->xyls));

    Nindexes = n_indexes(bp);
    use_serial_compat_shards =
        ((shard_mode == INDEX_SHARD_SERIAL_COMPAT) && !bp->indexes_inparallel);
    use_serial_reduce_shards =
        ((shard_mode == INDEX_SHARD_SERIAL_REDUCE) && !bp->indexes_inparallel);
    use_threaded_shards =
        ((shard_mode == INDEX_SHARD_THREADS) && !bp->indexes_inparallel);
    if (trace_index_shards) {
        logmsg("[index-shard] mode=%s active=%i indexes_inparallel=%i "
               "candidates=%zu job=%s workers=%i\n",
               index_shard_mode_name(shard_mode),
               (use_serial_compat_shards ||
                use_serial_reduce_shards ||
                use_threaded_shards) ? 1 : 0,
               bp->indexes_inparallel ? 1 : 0, Nindexes,
               index_shard_string_or_null(bp->fieldfname),
               (shard_mode == INDEX_SHARD_THREADS) ?
               index_shard_workers_from_env() : 0);
        for (I=0; I<Nindexes; I++) {
            logmsg("[index-shard] candidate order=%zu index=%s\n",
                   I, index_shard_string_or_null(get_index_name(bp, I)));
        }
        if ((shard_mode != INDEX_SHARD_OFF) &&
            bp->indexes_inparallel)
            logmsg("[index-shard] shard mode requested but "
                   "indexes_inparallel=1; keeping existing inparallel path\n");
    }

    // Verify any WCS estimates we have.
    if (bl_size(bp->verify_wcs_list)) {
        int i;
        int w;

        // We want to get the best logodds out of all the indices, so we set the
        // logodds-to-solve impossibly high so that a "good enough" solution doesn't
        // stop us from continuing to search...
        double oldodds = bp->logratio_tosolve;
        bp->logratio_tosolve = LARGE_VAL;

        for (w = 0; w < bl_size(bp->verify_wcs_list); w++) {
            double pixscale;
            double quadlo, quadhi;
            sip_t* wcs = bl_access(bp->verify_wcs_list, w);

            // We don't want to try to verify a wide-field image using a narrow-
            // field index, because it will contain a TON of index stars in the
            // field.  We therefore only try to verify using indices that contain
            // quads that could have been found in the image.
            if (wcs->wcstan.imagew == 0.0 && sp->field_maxx > 0.0)
                wcs->wcstan.imagew = sp->field_maxx;
            if (wcs->wcstan.imageh == 0.0 && sp->field_maxy > 0.0)
                wcs->wcstan.imageh = sp->field_maxy;

            if ((wcs->wcstan.imagew == 0) ||
                (wcs->wcstan.imageh == 0)) {
                logmsg("Verifying WCS: image width or height is zero / unknown.\n");
                continue;
            }
            pixscale = sip_pixel_scale(wcs);
            quadlo = bp->quad_size_fraction_lo
                * MIN(wcs->wcstan.imagew, wcs->wcstan.imageh)
                * pixscale;
            quadhi = bp->quad_size_fraction_hi
                * MAX(wcs->wcstan.imagew, wcs->wcstan.imageh)
                * pixscale;
            logmsg("Verifying WCS using indices with quads of size [%g, %g] arcmin\n",
                   arcsec2arcmin(quadlo), arcsec2arcmin(quadhi));

            for (I=0; I<Nindexes; I++) {
                index_t* index = get_index(bp, I);
                if (!index_overlaps_scale_range(index, quadlo, quadhi)) {
                    done_with_index(bp, I, index);
                    continue;
                }
                solver_add_index(sp, index);
                sp->index = index;
                logmsg("Verifying WCS with index %zu of %zu (%s)\n",  I + 1, Nindexes, index->indexname);
                // Do it!
                solve_fields(bp, wcs);
                // Clean up this index...
                done_with_index(bp, I, index);
                solver_clear_indexes(sp);
            }
        }

        bp->logratio_tosolve = oldodds;

        logmsg("Got %zu solutions.\n", bl_size(bp->solutions));

        if (bp->best_hit_only)
            remove_duplicate_solutions(bp);

        for (i=0; i<bl_size(bp->solutions); i++) {
            MatchObj* mo = bl_access(bp->solutions, i);
            if (mo->logodds >= bp->logratio_tosolve)
                solved_field(bp, mo->fieldnum);
        }
    }

    if (bp->single_field_solved)
        goto cleanup;

    // Start solving...
    if (bp->indexes_inparallel) {

        // Add all the indexes...
        for (I=0; I<Nindexes; I++) {
            index_t* index = get_index(bp, I);
            if (trace_index_shards)
                logmsg("[index-shard] processing order=%zu index=%s\n",
                       I, index_shard_string_or_null(index->indexname));
            solver_add_index(sp, index);
        }

        // Record current CPU usage.
        bp->cpu_start = get_cpu_usage();
        // Record current wall-clock time.
        bp->time_start = time(NULL);

        // Do it!
        solve_fields(bp, NULL);

        // Clean up the indices...
        for (I=0; I<Nindexes; I++) {
            index_t* index = get_index(bp, I);
            done_with_index(bp, I, index);
        }
        solver_clear_indexes(sp);

    } else if (use_serial_compat_shards) {
        if (onefield_run_index_shards_serial_compat(bp))
            exit(-1);

    } else if (use_serial_reduce_shards) {
        if (onefield_run_index_shards_serial_reduce(bp))
            exit(-1);

    } else if (use_threaded_shards) {
        if (onefield_run_index_shards_threaded_reduce(
                bp, index_shard_workers_from_env()))
            exit(-1);

    } else {

        for (I=0; I<Nindexes; I++) {
            if (onefield_index_loop_should_stop(bp))
                break;

            run_index_candidate(bp, sp, I, trace_index_shards, -1);
        }
    }

 cleanup:
    // Clean up.
    xylist_close(bp->xyls);

    if (write_solutions(bp))
        exit(-1);

    for (i=0; i<bl_size(bp->solutions); i++) {
        MatchObj* mo = bl_access(bp->solutions, i);
        verify_free_matchobj(mo);
        onefield_free_matchobj(mo);
    }
    bl_remove_all(bp->solutions);
}

void onefield_init(onefield_t* bp) {
    // Reset params.
    memset(bp, 0, sizeof(onefield_t));

    bp->fieldlist = il_new(256);
    bp->solutions = bl_new(16, sizeof(MatchObj));
    bp->indexnames = sl_new(16);
    bp->indexes = pl_new(16);
    bp->verify_wcs_list = bl_new(1, sizeof(sip_t));
    bp->verify_wcsfiles = sl_new(1);
    bp->fieldid_key = strdup("FIELDID");
    onefield_set_xcol(bp, NULL);
    onefield_set_ycol(bp, NULL);
    bp->quad_size_fraction_lo = DEFAULT_QSF_LO;
    bp->quad_size_fraction_hi = DEFAULT_QSF_HI;
    bp->nsolves = 1;

    bp->xyls_tagalong_all = TRUE;
    // don't set sp-> here because solver_set_default_values()
    // will get called next and wipe it out...
}

int onefield_parameters_are_okay(onefield_t* bp, solver_t* sp) {
    if (sp->distractor_ratio == 0) {
        logerr("You must set a \"distractors\" proportion.\n");
        return 0;
    }
    if (!(sl_size(bp->indexnames) || (bp->indexes_inparallel && pl_size(bp->indexes)))) {
        logerr("You must specify one or more indexes.\n");
        return 0;
    }
    if (!bp->fieldfname) {
        logerr("You must specify a field filename (xylist).\n");
        return 0;
    }
    if (sp->codetol < 0.0) {
        logerr("You must specify codetol > 0\n");
        return 0;
    }
    if (sp->verify_pix <= 0.0) {
        logerr("You must specify a positive verify_pix.\n");
        return 0;
    }
    if ((sp->funits_lower != 0.0) && (sp->funits_upper != 0.0) &&
        (sp->funits_lower > sp->funits_upper)) {
        logerr("fieldunits_lower MUST be less than fieldunits_upper.\n");
        logerr("\n(in other words, the lower-bound of scale estimate must "
               "be less than the upper-bound!)\n\n");
        return 0;
    }
    return 1;
}

int onefield_is_run_obsolete(onefield_t* bp, solver_t* sp) {
    // If we're just solving one field, check to see if it's already
    // solved before doing a bunch of work and spewing tons of output.
    if ((il_size(bp->fieldlist) == 1) && bp->solved_in) {
        if (is_field_solved(bp, il_get(bp->fieldlist, 0)))
            return 1;
    }
    // Early check to see if this job was cancelled.
    if (bp->cancelfname) {
        if (file_exists(bp->cancelfname)) {
            logerr("Run cancelled.\n");
            return 1;
        }
    }

    return 0;
}

static void load_and_parse_wcsfiles(onefield_t* bp) {
    int i;
    for (i = 0; i < sl_size(bp->verify_wcsfiles); i++) {
        sip_t wcs;
        char* fn = sl_get(bp->verify_wcsfiles, i);
        logmsg("Reading WCS header to verify from file %s\n", fn);
        memset(&wcs, 0, sizeof(sip_t));
        if (!sip_read_header_file(fn, &wcs)) {
            logerr("Failed to parse WCS header from file %s\n", fn);
            continue;
        }
        bl_append(bp->verify_wcs_list, &wcs);
    }
}

void onefield_log_run_parameters(onefield_t* bp) {
    solver_t* sp = &(bp->solver);
    int i, N;

    logverb("solver run parameters:\n");
    logverb("indexes:\n");
    N = n_indexes(bp);
    for (i=0; i<N; i++)
        logverb("  %s\n", get_index_name(bp, i));
    if (bp->fieldfname)
        logverb("fieldfname %s\n", bp->fieldfname);
    logverb("fields ");
    for (i = 0; i < il_size(bp->fieldlist); i++)
        logverb("%i ", il_get(bp->fieldlist, i));
    logverb("\n");
    for (i = 0; i < sl_size(bp->verify_wcsfiles); i++)
        logverb("verify %s\n", sl_get(bp->verify_wcsfiles, i));
    logverb("fieldid %i\n", bp->fieldid);
    if (bp->matchfname)
        logverb("matchfname %s\n", bp->matchfname);
    if (bp->solved_in)
        logverb("solved_in %s\n", bp->solved_in);
    if (bp->solved_out)
        logverb("solved_out %s\n", bp->solved_out);
    if (bp->cancelfname)
        logverb("cancel %s\n", bp->cancelfname);
    if (bp->wcs_template)
        logverb("wcs %s\n", bp->wcs_template);
    if (bp->fieldid_key)
        logverb("fieldid_key %s\n", bp->fieldid_key);
    if (bp->indexrdlsfname)
        logverb("indexrdlsfname %s\n", bp->indexrdlsfname);
    logverb("parity %i\n", sp->parity);
    logverb("codetol %g\n", sp->codetol);
    logverb("startdepth %i\n", sp->startobj);
    logverb("enddepth %i\n", sp->endobj);
    logverb("fieldunits_lower %g\n", sp->funits_lower);
    logverb("fieldunits_upper %g\n", sp->funits_upper);
    logverb("verify_pix %g\n", sp->verify_pix);
    if (bp->xcolname)
        logverb("xcolname %s\n", bp->xcolname);
    if (bp->ycolname)
        logverb("ycolname %s\n", bp->ycolname);
    logverb("maxquads %i\n", sp->maxquads);
    logverb("maxmatches %i\n", sp->maxmatches);
    logverb("cpulimit %f\n", bp->cpulimit);
    logverb("timelimit %i\n", bp->timelimit);
    logverb("total_timelimit %g\n", bp->total_timelimit);
    logverb("total_cpulimit %f\n", bp->total_cpulimit);
}

void onefield_cleanup(onefield_t* bp) {
    il_free(bp->fieldlist);
    bl_free(bp->solutions);
    sl_free2(bp->indexnames);
    pl_free(bp->indexes);
    sl_free2(bp->verify_wcsfiles);
    bl_free(bp->verify_wcs_list);
    sl_free2(bp->rdls_tagalong);

    free(bp->cancelfname);
    free(bp->fieldfname);
    free(bp->fieldid_key);
    free(bp->indexrdlsfname);
    free(bp->scamp_fname);
    free(bp->corr_fname);
    free(bp->matchfname);
    free(bp->solved_in);
    free(bp->solved_out);
    free(bp->wcs_template);
    free(bp->xcolname);
    free(bp->ycolname);
    free(bp->sort_rdls);
}

static int sort_rdls(MatchObj* mymo, onefield_t* bp) {
    const solver_t* sp = &(bp->solver);
    anbool asc = TRUE;
    char* colname = bp->sort_rdls;
    double* sortdata;
    fitstable_t* tagalong;
    int* perm;
    int i;
    logverb("Sorting RDLS by column \"%s\"\n", bp->sort_rdls);
    if (colname[0] == '-') {
        colname++;
        asc = FALSE;
    }
    tagalong = startree_get_tagalong(sp->index->starkd);
    if (!tagalong) {
        ERROR("Failed to find tag-along table in index");
        return -1;
    }
    sortdata = fitstable_read_column_inds(tagalong, colname, fitscolumn_double_type(),
                                          mymo->refstarid, mymo->nindex);
    if (!sortdata) {
        ERROR("Failed to read data for column \"%s\" in index", colname);
        return -1;
    }
    perm = permutation_init(NULL, mymo->nindex);
    permuted_sort(sortdata, sizeof(double), asc ? compare_doubles_asc : compare_doubles_desc,
                  perm, mymo->nindex);
    free(sortdata);

    if (mymo->refxyz)
        permutation_apply(perm, mymo->nindex, mymo->refxyz, mymo->refxyz, 3*sizeof(double));
    // probably not set yet, but what the heck...
    if (mymo->refradec)
        permutation_apply(perm, mymo->nindex, mymo->refradec,  mymo->refradec, 2*sizeof(double));
    if (mymo->refxy)
        permutation_apply(perm, mymo->nindex, mymo->refxy,     mymo->refxy,    2*sizeof(double));
    if (mymo->refstarid)
        permutation_apply(perm, mymo->nindex, mymo->refstarid, mymo->refstarid,  sizeof(int));
    if (mymo->theta)
        for (i=0; i<mymo->nfield; i++) {
            if (mymo->theta[i] < 0)
                continue;
            mymo->theta[i] = perm[mymo->theta[i]];
        }
    free(perm);
    return 0;
}

static anbool record_match_common(MatchObj* mo, record_match_context_t* ctx) {
    onefield_t* bp = ctx->bp;
    solver_t* sp = ctx->sp;
    MatchObj* mymo;
    int ind;

    check_time_limits_for_solver(bp, sp);

    // Copy "mo" to "mymo".
    ind = bl_insert_sorted(ctx->solutions, mo, compare_matchobjs);
    mymo = bl_access(ctx->solutions, ind);

    // steal these arrays from "mo" (prevent them from being free()'d
    // by the caller)
    mo->theta = NULL;
    mo->matchodds = NULL;
    mo->refxyz = NULL;
    mo->refxy = NULL;
    mo->refstarid = NULL;
    mo->testperm = NULL;

    // We have no guarantee that the index will still be open when it
    // comes time to write our output files, so we've got to grab everything
    // we need now while it's at hand.

    if (bp->indexrdlsfname || bp->scamp_fname || bp->corr_fname) {
        int i;

        // This must happen first, because it reorders the "ref" arrays,
        // and we want that to be done before more data are integrated.
        if (bp->sort_rdls) {
            if (sort_rdls(mymo, bp)) {
                ERROR("Failed to sort RDLS file by column \"%s\"", bp->sort_rdls);
            }
        }

        logdebug("Converting %i reference stars from xyz to radec\n", mymo->nindex);
        mymo->refradec = malloc(mymo->nindex * 2 * sizeof(double));
        for (i=0; i<mymo->nindex; i++) {
            xyzarr2radecdegarr(mymo->refxyz+i*3, mymo->refradec+i*2);
            logdebug("  %i: radec %.2f,%.2f\n", i, mymo->refradec[i*2], mymo->refradec[i*2+1]);
        }

        mymo->fieldxy = malloc(mymo->nfield * 2 * sizeof(double));
        // whew!
        memcpy(mymo->fieldxy, sp->vf->xy, mymo->nfield * 2 * sizeof(double));

        // Tweak was here...

        // FIXME -- add MAG, MAGERR, and positional errors for SCAMP catalog.

        if (bp->rdls_tagalong || bp->rdls_tagalong_all)
            grab_tagalong_data(sp->index->starkd, mymo, bp, mymo->refstarid, mymo->nindex);

        // FIXME -- we don't support specifying individual fields (yet)
        assert(bp->xyls_tagalong_all);
        assert(!bp->xyls_tagalong);
        if (bp->xyls_tagalong_all)
            grab_field_tagalong_data(mymo, bp->xyls, mymo->nfield);
    }

    if (mymo->logodds < bp->logratio_tosolve)
        return FALSE;

    // this match is considered a solution.

    (*(ctx->nsolves_sofar))++;
    if (*(ctx->nsolves_sofar) < bp->nsolves) {
        logmsg("Found a quad that solves the image; that makes %i of %i required.\n",
               *(ctx->nsolves_sofar), bp->nsolves);
    } else {
        if (sp->index) {
            char* base = basename_safe(sp->index->indexname);
            logmsg("Field %i: solved with index %s.\n", mymo->fieldnum, base);
            free(base);
        } else {
            logmsg("Field %i: solved with index %i", mymo->fieldnum, mymo->indexid);
            if (mymo->healpix >= 0)
                logmsg(", healpix %i\n", mymo->healpix);
            else
                logmsg("\n");
        }
        return TRUE;
    }
    return FALSE;
}

static anbool record_match_callback(MatchObj* mo, void* userdata) {
    onefield_t* bp = userdata;
    record_match_context_t ctx;
    ctx.bp = bp;
    ctx.sp = &(bp->solver);
    ctx.solutions = bp->solutions;
    ctx.nsolves_sofar = &(bp->nsolves_sofar);
    return record_match_common(mo, &ctx);
}

static anbool record_match_shard_callback(MatchObj* mo, void* userdata) {
    record_match_context_t* ctx = userdata;
    return record_match_common(mo, ctx);
}

static time_t timer_callback(void* user_data) {
    onefield_t* bp = user_data;

    check_time_limits(bp);

    // check if the field has already been solved...
    if (is_field_solved(bp, bp->fieldnum))
        return 0;
    if (bp->cancelfname && file_exists(bp->cancelfname)) {
        bp->cancelled = TRUE;
        logmsg("File \"%s\" exists: cancelling.\n", bp->cancelfname);
        return 0;
    }
    return 1; // wait 1 second... FIXME config?
}

static time_t timer_shard_callback(void* user_data) {
    record_match_context_t* ctx = user_data;
    onefield_t* bp = ctx->bp;

    check_time_limits_for_solver(bp, ctx->sp);

    // check if the field has already been solved...
    if (is_field_solved(bp, bp->fieldnum))
        return 0;
    if (bp->cancelfname && file_exists(bp->cancelfname)) {
        bp->cancelled = TRUE;
        logmsg("File \"%s\" exists: cancelling.\n", bp->cancelfname);
        return 0;
    }
    return 1; // wait 1 second... FIXME config?
}

static void add_onefield_params(onefield_t* bp, qfits_header* hdr) {
    solver_t* sp = &(bp->solver);
    int i;
    int Nindexes;
    fits_add_long_comment(hdr, "-- onefield solver parameters: --");
    if (sp->index) {
        fits_add_long_comment(hdr, "Index name: %s", sp->index->indexname?sp->index->indexname:"(null)");
        fits_add_long_comment(hdr, "Index id: %i", sp->index->indexid);
        fits_add_long_comment(hdr, "Index healpix: %i", sp->index->healpix);
        fits_add_long_comment(hdr, "Index healpix nside: %i", sp->index->hpnside);
        fits_add_long_comment(hdr, "Index scale lower: %g arcsec", sp->index->index_scale_lower);
        fits_add_long_comment(hdr, "Index scale upper: %g arcsec", sp->index->index_scale_upper);
        fits_add_long_comment(hdr, "Index jitter: %g", sp->index->index_jitter);
        fits_add_long_comment(hdr, "Circle: %s", sp->index->circle ? "yes" : "no");
        fits_add_long_comment(hdr, "Cxdx margin: %g", sp->cxdx_margin);
    }
    Nindexes = n_indexes(bp);
    for (i = 0; i < Nindexes; i++)
        fits_add_long_comment(hdr, "Index(%i): %s", i, get_index_name(bp, i)?get_index_name(bp, i):"(null)");

    fits_add_long_comment(hdr, "Field name: %s", bp->fieldfname?bp->fieldfname:"(null)");
    fits_add_long_comment(hdr, "Field scale lower: %g arcsec/pixel", sp->funits_lower);
    fits_add_long_comment(hdr, "Field scale upper: %g arcsec/pixel", sp->funits_upper);
    fits_add_long_comment(hdr, "X col name: %s", bp->xcolname?bp->xcolname:"(null)");
    fits_add_long_comment(hdr, "Y col name: %s", bp->ycolname?bp->ycolname:"(null)");
    fits_add_long_comment(hdr, "Start obj: %i", sp->startobj);
    fits_add_long_comment(hdr, "End obj: %i", sp->endobj);
	
    // 'Solved_in' is often a NULL pointer.
    // If %s is a NULL pointer, vasprintf() causes a segmentation fault (due to strlen()) on Solaris -> added treatment of this case for portability. 
    // GNU/Linux implementation of vasprintf() catches NULL pointer and prints "(null)" in header. Seems to be an issue on Solaris only.
    fits_add_long_comment(hdr, "Solved_in: %s", bp->solved_in?bp->solved_in:"(null)");
    fits_add_long_comment(hdr, "Solved_out: %s", bp->solved_out?bp->solved_out:"(null)");

    fits_add_long_comment(hdr, "Parity: %i", sp->parity);
    fits_add_long_comment(hdr, "Codetol: %g", sp->codetol);
    fits_add_long_comment(hdr, "Verify pixels: %g pix", sp->verify_pix);

    fits_add_long_comment(hdr, "Maxquads: %i", sp->maxquads);
    fits_add_long_comment(hdr, "Maxmatches: %i", sp->maxmatches);
    fits_add_long_comment(hdr, "Cpu limit: %f s", bp->cpulimit);
    fits_add_long_comment(hdr, "Time limit: %i s", bp->timelimit);
    fits_add_long_comment(hdr, "Total time limit: %g s", bp->total_timelimit);
    fits_add_long_comment(hdr, "Total CPU limit: %f s", bp->total_cpulimit);

    fits_add_long_comment(hdr, "Tweak: %s", (sp->do_tweak ? "yes" : "no"));
    if (sp->do_tweak) {
        fits_add_long_comment(hdr, "Tweak AB order: %i", sp->tweak_aborder);
        fits_add_long_comment(hdr, "Tweak ABP order: %i", sp->tweak_abporder);
    }

    fits_add_long_comment(hdr, "--");
}

static void remove_invalid_fields(il* fieldlist, int maxfield) {
    int i;
    for (i=0; i<il_size(fieldlist); i++) {
        int fieldnum = il_get(fieldlist, i);
        if (fieldnum >= 1 && fieldnum <= maxfield)
            continue;
        if (fieldnum > maxfield) {
            logerr("Field %i does not exist (max=%i).\n", fieldnum, maxfield);
        }
        if (fieldnum < 1) {
            logerr("Field %i is invalid (must be >= 1).\n", fieldnum);
        }
        il_remove(fieldlist, i);
        i--;
    }
}

static void solve_fields(onefield_t* bp, sip_t* verify_wcs) {
    solve_fields_with_solver(bp, &(bp->solver), verify_wcs, bp);
}

static void solve_fields_with_solver(onefield_t* bp, solver_t* sp,
                                     sip_t* verify_wcs,
                                     void* callback_userdata) {
    solve_fields_with_solver_and_metrics(bp, sp, verify_wcs,
                                         callback_userdata, NULL);
}

static void solve_fields_with_solver_and_metrics(
    onefield_t* bp, solver_t* sp, sip_t* verify_wcs,
    void* callback_userdata, onefield_solve_metric_context_t* metric_ctx) {
    double last_utime, last_stime;
    double utime, stime;
    struct timeval wtime, last_wtime;
    int fi;

    get_resource_stats(&last_utime, &last_stime, NULL);
    gettimeofday(&last_wtime, NULL);

    for (fi = 0; fi < il_size(bp->fieldlist); fi++) {
        int fieldnum;
        MatchObj template ;
        qfits_header* fieldhdr = NULL;
        double metric_solve_t0 = 0.0;

        fieldnum = il_get(bp->fieldlist, fi);

        memset(&template, 0, sizeof(MatchObj));
        template.fieldnum = fieldnum;
        template.fieldfile = bp->fieldid;

        // Get the FIELDID string from the xyls FITS header.
        if (xylist_open_field(bp->xyls, fieldnum)) {
            logerr("Failed to open extension %i in xylist.\n", fieldnum);
            goto cleanup;
        }
        fieldhdr = xylist_get_header(bp->xyls);
        if (fieldhdr) {
            char* idstr = fits_get_dupstring(fieldhdr, bp->fieldid_key);
            if (idstr)
                strncpy(template.fieldname, idstr, sizeof(template.fieldname) - 1);
            free(idstr);
        }

        // Has the field already been solved?
        if (is_field_solved(bp, fieldnum))
            goto cleanup;

        // Get the field.
        solver_set_field(sp, xylist_read_field(bp->xyls, NULL));
        if (!sp->fieldxy_orig) {
            logerr("Failed to read xylist field.\n");
            goto cleanup;
        }

        solver_reset_counters(sp);
        solver_reset_best_match(sp);

        sp->mo_template = &template;
        if (sp == &(bp->solver)) {
            sp->record_match_callback = record_match_callback;
            sp->timer_callback = timer_callback;
        } else {
            sp->record_match_callback = record_match_shard_callback;
            sp->timer_callback = timer_shard_callback;
        }
        sp->userdata = callback_userdata;

        bp->fieldnum = fieldnum;
        bp->nsolves_sofar = 0;
        if (sp != &(bp->solver)) {
            record_match_context_t* ctx = callback_userdata;
            if (ctx && ctx->nsolves_sofar)
                *(ctx->nsolves_sofar) = 0;
        }

        if (metric_ctx)
            metric_solve_t0 = timenow();
        solver_preprocess_field(sp);

        if (verify_wcs) {
            //MatchObj mo;
            logmsg("Verifying WCS of field %i.\n", fieldnum);
            solver_verify_sip_wcs(sp, verify_wcs); //, &mo);
            logmsg(" --> log-odds %g\n", sp->best_logodds);

        } else {
            logverb("Solving field %i.\n", fieldnum);
            sp->distance_from_quad_bonus = TRUE;
            solver_log_params(sp);

            // The real thing
            solver_run(sp);

            logverb("Field %i: tried %i quads, matched %i codes.\n",
                    fieldnum, sp->numtries, sp->nummatches);

            if (sp->maxquads && sp->numtries >= sp->maxquads)
                logmsg("  exceeded the number of quads to try: %i >= %i.\n",
                       sp->numtries, sp->maxquads);
            if (sp->maxmatches && sp->nummatches >= sp->maxmatches)
                logmsg("  exceeded the number of quads to match: %i >= %i.\n",
                       sp->nummatches, sp->maxmatches);
            if (bp->cancelled)
                logmsg("  cancelled at user request.\n");
        }


        if (sp->best_match_solves) {
            solved_field(bp, fieldnum);
        } else if (!verify_wcs) {
            // Field unsolved.
            if (sp->index && sp->index->indexname) {
                char* copy;
                char* base;
                copy = strdup(sp->index->indexname);
                base = strdup(basename(copy));
                free(copy);
                if (sp->endobj)
                    logerr("Field %i did not solve (index %s, "
                           "field objects %i-%i).\n",
                           fieldnum, base, sp->startobj+1, sp->endobj);
                else
                    logerr("Field %i did not solve (index %s).\n",
                           fieldnum, base);
                free(base);
            } else {
                logerr("Field %i did not solve.\n", fieldnum);
            }
            if (sp->have_best_match) {
                logverb("Best match encountered: ");
                matchobj_print(&(sp->best_match), log_get_level());
            } else {
                logverb("Best odds encountered: %g\n", exp(sp->best_logodds));
            }
        }

        if (metric_ctx) {
            double solve_ms = 1000.0 * (timenow() - metric_solve_t0);
            anbool solved = sp->best_match_solves ||
                (!metric_ctx->solved_before && bp->single_field_solved);
            onefield_write_index_metric_for_solver(
                bp, sp, metric_ctx->shard_id, metric_ctx->index_order,
                metric_ctx->index, metric_ctx->load_ms, solve_ms, solved,
                onefield_index_exit_reason(bp, sp));
        }

        solver_free_field(sp);

        get_resource_stats(&utime, &stime, NULL);
        gettimeofday(&wtime, NULL);
        logverb("Spent %g s user, %g s system, %g s total, %g s wall time.\n",
                (utime - last_utime), (stime - last_stime),
                (stime - last_stime + utime - last_utime),
                millis_between(&last_wtime, &wtime) * 0.001);

        last_utime = utime;
        last_stime = stime;
        last_wtime = wtime;

    cleanup:
        solver_cleanup_field(sp);
    }
}

static anbool is_field_solved(onefield_t* bp, int fieldnum) {
    anbool solved = FALSE;
    if (bp->solved_in) {
        solved = solvedfile_get(bp->solved_in, fieldnum);
        logverb("Checking %s file %i to see if the field is solved: %s.\n",
                bp->solved_in, fieldnum, (solved ? "yes" : "no"));
    }
    if (solved) {
        // file exists; field has already been solved.
        logmsg("Field %i: solvedfile %s: field has been solved.\n", fieldnum, bp->solved_in);
        return TRUE;
    }
    return FALSE;
}

static void solved_field(onefield_t* bp, int fieldnum) {
    // Record in solved file, or send to solved server.
    if (bp->solved_out) {
        logmsg("Field %i solved: writing to file %s to indicate this.\n", fieldnum, bp->solved_out);
        if (solvedfile_set(bp->solved_out, fieldnum)) {
            logerr("Failed to write solvedfile %s.\n", bp->solved_out);
        }
    }
    // If we're just solving a single field, and we solved it...
    if (il_size(bp->fieldlist) == 1)
        bp->single_field_solved = TRUE;
}

static bl* copy_tagalong_list(bl* src) {
    bl* dst;
    int i;
    if (!src)
        return NULL;
    dst = bl_new(16, sizeof(tagalong_t));
    for (i=0; i<bl_size(src); i++) {
        tagalong_t* tag = bl_access(src, i);
        tagalong_t tagcopy;
        memcpy(&tagcopy, tag, sizeof(tagalong_t));
        tagcopy.name = strdup_safe(tag->name);
        tagcopy.units = strdup_safe(tag->units);
        if (tag->data) {
            tagcopy.data = malloc((size_t)tag->Ndata * (size_t)tag->itemsize);
            memcpy(tagcopy.data, tag->data, (size_t)tag->Ndata * (size_t)tag->itemsize);
        }
        bl_append(dst, &tagcopy);
    }
    return dst;
}

void onefield_matchobj_deep_copy(const MatchObj* mo, MatchObj* dest) {
    if (!mo || !dest)
        return;
    if (mo->sip) {
        dest->sip = sip_create();
        memcpy(dest->sip, mo->sip, sizeof(sip_t));
    }
    if (mo->refradec) {
        dest->refradec = malloc(mo->nindex * 2 * sizeof(double));
        memcpy(dest->refradec, mo->refradec, mo->nindex * 2 * sizeof(double));
    }
    if (mo->fieldxy) {
        dest->fieldxy = malloc(mo->nfield * 2 * sizeof(double));
        memcpy(dest->fieldxy, mo->fieldxy, mo->nfield * 2 * sizeof(double));
    }
    dest->tagalong = copy_tagalong_list(mo->tagalong);
    dest->field_tagalong = copy_tagalong_list(mo->field_tagalong);
}

static void matchobj_deep_copy_all(const MatchObj* mo, MatchObj* dest) {
    memcpy(dest, mo, sizeof(MatchObj));
    dest->sip = NULL;
    dest->refradec = NULL;
    dest->fieldxy = NULL;
    dest->fieldxy_orig = NULL;
    dest->tagalong = NULL;
    dest->field_tagalong = NULL;
    dest->theta = NULL;
    dest->matchodds = NULL;
    dest->testperm = NULL;
    dest->refxyz = NULL;
    dest->refxy = NULL;
    dest->refstarid = NULL;
    onefield_matchobj_deep_copy(mo, dest);
    verify_matchobj_deep_copy(mo, dest);
}

// Free the things I added to the mo.
void onefield_free_matchobj(MatchObj* mo) {
    if (!mo) return;
    if (mo->sip) {
        sip_free(mo->sip);
        mo->sip = NULL;
    }
    free(mo->refradec);
    free(mo->fieldxy);
    free(mo->theta);
    free(mo->matchodds);
    free(mo->refxyz);
    free(mo->refxy);
    free(mo->refstarid);
    free(mo->testperm);
    mo->refradec = NULL;
    mo->fieldxy = NULL;
    mo->theta = NULL;
    mo->matchodds = NULL;
    mo->refxyz = NULL;
    mo->refxy = NULL;
    mo->refstarid = NULL;
    mo->testperm = NULL;

    if (mo->tagalong) {
        int i;
        for (i=0; i<bl_size(mo->tagalong); i++) {
            tagalong_t* tag = bl_access(mo->tagalong, i);
            free(tag->name);
            free(tag->units);
            free(tag->data);
        }
        bl_free(mo->tagalong);
        mo->tagalong = NULL;
    }
    if (mo->field_tagalong) {
        int i;
        for (i=0; i<bl_size(mo->field_tagalong); i++) {
            tagalong_t* tag = bl_access(mo->field_tagalong, i);
            free(tag->name);
            free(tag->units);
            free(tag->data);
        }
        bl_free(mo->field_tagalong);
        mo->field_tagalong = NULL;
    }
}

static void remove_duplicate_solutions(onefield_t* bp) {
    int i, j;
    // The solutions can fall out of order because tweak2() updates their logodds.
    bl_sort(bp->solutions, compare_matchobjs);

    for (i=0; i<bl_size(bp->solutions); i++) {
        MatchObj* mo = bl_access(bp->solutions, i);
        j = i+1;
        while (j < bl_size(bp->solutions)) {
            MatchObj* mo2 = bl_access(bp->solutions, j);
            if (mo->fieldfile != mo2->fieldfile)
                break;
            if (mo->fieldnum != mo2->fieldnum)
                break;
            assert(mo2->logodds <= mo->logodds);
            onefield_free_matchobj(mo2);
            verify_free_matchobj(mo2);
            bl_remove_index(bp->solutions, j);
        }
    }
}

static int write_match_file(onefield_t* bp) {
    int i;
    bp->mf = matchfile_open_for_writing(bp->matchfname);
    if (!bp->mf) {
        logerr("Failed to open file %s to write match file.\n", bp->matchfname);
        return -1;
    }
    BOILERPLATE_ADD_FITS_HEADERS(bp->mf->header);
    qfits_header_add(bp->mf->header, "DATE", qfits_get_datetime_iso8601(), "Date this file was created.", NULL);
    add_onefield_params(bp, bp->mf->header);
    if (matchfile_write_headers(bp->mf)) {
        logerr("Failed to write matchfile header.\n");
        return -1;
    }
    for (i=0; i<bl_size(bp->solutions); i++) {
        MatchObj* mo = bl_access(bp->solutions, i);
        if (matchfile_write_match(bp->mf, mo)) {
            logerr("Field %i: error writing a match.\n", mo->fieldnum);
            return -1;
        }
    }
    if (matchfile_fix_headers(bp->mf) ||
        matchfile_close(bp->mf)) {
        logerr("Error closing matchfile.\n");
        return -1;
    }
    bp->mf = NULL;
    return 0;
}

static int write_rdls_file(onefield_t* bp) {
    int i;
    qfits_header* h;
    bp->indexrdls = rdlist_open_for_writing(bp->indexrdlsfname);
    if (!bp->indexrdls) {
        logerr("Failed to open index RDLS file %s for writing.\n",
               bp->indexrdlsfname);
        return -1;
    }
    h = rdlist_get_primary_header(bp->indexrdls);

    BOILERPLATE_ADD_FITS_HEADERS(h);
    fits_add_long_history(h, "This \"indexrdls\" file contains the RA/DEC of index objects that were found inside a solved field.");
    qfits_header_add(h, "DATE", qfits_get_datetime_iso8601(), "Date this file was created.", NULL);
    add_onefield_params(bp, h);
    if (rdlist_write_primary_header(bp->indexrdls)) {
        logerr("Failed to write index RDLS header.\n");
        return -1;
    }

    for (i=0; i<bl_size(bp->solutions); i++) {
        MatchObj* mo = bl_access(bp->solutions, i);
        rd_t rd;
        if (strlen(mo->fieldname)) {
            qfits_header* hdr = rdlist_get_header(bp->indexrdls);
            qfits_header_add(hdr, "FIELDID", mo->fieldname, "Name of this field", NULL);
        }
        if (mo->tagalong) {
            int j;
            for (j=0; j<bl_size(mo->tagalong); j++) {
                tagalong_t* tag = bl_access(mo->tagalong, j);
                tag->colnum = rdlist_add_tagalong_column(bp->indexrdls, tag->type, tag->arraysize,
                                                         tag->type, tag->name, tag->units);
            }
        }
        if (rdlist_write_header(bp->indexrdls)) {
            logerr("Failed to write index RDLS field header.\n");
            return -1;
        }
        assert(mo->refradec);

        rd_from_array(&rd, mo->refradec, mo->nindex);
        if (rdlist_write_field(bp->indexrdls, &rd)) {
            logerr("Failed to write index RDLS entry.\n");
            return -1;
        }
        rd_free_data(&rd);

        if (mo->tagalong) {
            int j;
            for (j=0; j<bl_size(mo->tagalong); j++) {
                tagalong_t* tag = bl_access(mo->tagalong, j);
                if (rdlist_write_tagalong_column(bp->indexrdls, tag->colnum,
                                                 0, mo->nindex, tag->data, tag->itemsize)) {
                    ERROR("Failed to write tag-along data column %s", tag->name);
                    return -1;
                }
            }
        }

        if (rdlist_fix_header(bp->indexrdls)) {
            logerr("Failed to fix index RDLS field header.\n");
            return -1;
        }
        rdlist_next_field(bp->indexrdls);
    }

    if (rdlist_fix_primary_header(bp->indexrdls) ||
        rdlist_close(bp->indexrdls)) {
        logerr("Failed to close index RDLS file.\n");
        return -1;
    }
    bp->indexrdls = NULL;
    return 0;
}

static int write_wcs_file(onefield_t* bp) {
    int i;
    for (i=0; i<bl_size(bp->solutions); i++) {
        char wcs_fn[1024];
        FILE* fout;
        qfits_header* hdr;
        char* tm;

        MatchObj* mo = bl_access(bp->solutions, i);
        snprintf(wcs_fn, sizeof(wcs_fn), bp->wcs_template, mo->fieldnum);
        fout = fopen(wcs_fn, "wb");
        if (!fout) {
            logerr("Failed to open WCS output file %s: %s\n", wcs_fn, strerror(errno));
            return -1;
        }
        assert(mo->wcs_valid);

        if (mo->sip)
            hdr = sip_create_header(mo->sip);
        else
            hdr = tan_create_header(&(mo->wcstan));

        BOILERPLATE_ADD_FITS_HEADERS(hdr);
        qfits_header_add(hdr, "HISTORY", "This is a WCS header was created by Astrometry.net.", NULL, NULL);
        tm = qfits_get_datetime_iso8601();
        qfits_header_add(hdr, "DATE", tm, "Date this file was created.", NULL);
        add_onefield_params(bp, hdr);
        fits_add_long_comment(hdr, "-- properties of the matching quad: --");
        fits_add_long_comment(hdr, "index id: %i", mo->indexid);
        fits_add_long_comment(hdr, "index healpix: %i", mo->healpix);
        fits_add_long_comment(hdr, "index hpnside: %i", mo->hpnside);
        fits_add_long_comment(hdr, "log odds: %g", mo->logodds);
        fits_add_long_comment(hdr, "odds: %g", exp(mo->logodds));
        fits_add_long_comment(hdr, "quadno: %i", mo->quadno);
        fits_add_long_comment(hdr, "stars: %i,%i,%i,%i", mo->star[0], mo->star[1], mo->star[2], mo->star[3]);
        fits_add_long_comment(hdr, "field: %i,%i,%i,%i", mo->field[0], mo->field[1], mo->field[2], mo->field[3]);
        fits_add_long_comment(hdr, "code error: %g", sqrt(mo->code_err));
        fits_add_long_comment(hdr, "nmatch: %i", mo->nmatch);
        fits_add_long_comment(hdr, "nconflict: %i", mo->nconflict);
        fits_add_long_comment(hdr, "nfield: %i", mo->nfield);
        fits_add_long_comment(hdr, "nindex: %i", mo->nindex);
        fits_add_long_comment(hdr, "scale: %g arcsec/pix", mo->scale);
        fits_add_long_comment(hdr, "parity: %i", (int)mo->parity);
        fits_add_long_comment(hdr, "quads tried: %i", mo->quads_tried);
        fits_add_long_comment(hdr, "quads matched: %i", mo->quads_matched);
        fits_add_long_comment(hdr, "quads verified: %i", mo->nverified);
        fits_add_long_comment(hdr, "objs tried: %i", mo->objs_tried);
        fits_add_long_comment(hdr, "cpu time: %g", mo->timeused);
        fits_add_long_comment(hdr, "--");

        if (strlen(mo->fieldname))
            qfits_header_add(hdr, bp->fieldid_key, mo->fieldname, "Field name (copied from input field)", NULL);
			
        if (qfits_header_dump(hdr, fout)) {
            logerr("Failed to write FITS WCS header.\n");
            return -1;
        }
        fits_pad_file(fout);
        qfits_header_destroy(hdr);
        fclose(fout);
    }
    return 0;
}

static int write_scamp_file(onefield_t* bp) {
    int i;
    scamp_cat_t* scamp;
    qfits_header* hdr = NULL;
    MatchObj* mo;
    tan_t fakewcs;

    // HACK -- just hdr = NULL?
    hdr = qfits_header_default();
    fits_header_add_int(hdr, "BITPIX", 0, NULL);
    fits_header_add_int(hdr, "NAXIS", 2, NULL);
    fits_header_add_int(hdr, "NAXIS1", 0, NULL);
    fits_header_add_int(hdr, "NAXIS2", 0, NULL);
    qfits_header_add(hdr, "EXTEND", "T", "", NULL);
    memset(&fakewcs, 0, sizeof(tan_t));
    tan_add_to_header(hdr, &fakewcs);

    scamp = scamp_catalog_open_for_writing(bp->scamp_fname, TRUE);
    if (!scamp) {
        logerr("Failed to open SCAMP reference catalog for writing.\n");
        return -1;
    }
    if (scamp_catalog_write_field_header(scamp, hdr)) {
        logerr("Failed to write SCAMP headers.\n");
        return -1;
    }
    mo = bl_access(bp->solutions, 0);
    for (i=0; i<mo->nindex; i++) {
        scamp_ref_t ref;
        ref.ra  = mo->refradec[2*i + 0];
        ref.dec = mo->refradec[2*i + 1];
        ref.err_a = ref.err_b = arcsec2deg(mo->index_jitter);
        // HACK
        ref.mag = 10.0;
        ref.err_mag = 0.1;

        if (scamp_catalog_write_reference(scamp, &ref)) {
            logerr("Failed to write SCAMP object.\n");
            return -1;
        }
    }
    if (scamp_catalog_close(scamp)) {
        logerr("Failed to close SCAMP reference catalog.\n");
        return -1;
    }
    return 0;
}

static int write_corr_file(onefield_t* bp) {
    int i;
    fitstable_t* tab;
    tab = fitstable_open_for_writing(bp->corr_fname);
    if (!tab) {
        ERROR("Failed to open correspondences file \"%s\" for writing", bp->corr_fname);
        return -1;
    }
    // FIXME -- add header boilerplate.

    if (fitstable_write_primary_header(tab)) {
        ERROR("Failed to write primary header for corr file \"%s\"", bp->corr_fname);
        return -1;
    }

    for (i=0; i<bl_size(bp->solutions); i++) {
        MatchObj* mo;
        sip_t thesip;
        sip_t* wcs;
        int j;
        tfits_type dubl = fitscolumn_double_type();
        tfits_type itype = fitscolumn_int_type();

        mo = bl_access(bp->solutions, i);

        if (mo->sip)
            wcs = mo->sip;
        else {
            sip_wrap_tan(&mo->wcstan, &thesip);
            wcs = &thesip;
        }

        fitstable_add_write_column(tab, dubl, "field_x",   "pixels");
        fitstable_add_write_column(tab, dubl, "field_y",   "pixels");
        fitstable_add_write_column(tab, dubl, "field_ra",  "degrees");
        fitstable_add_write_column(tab, dubl, "field_dec", "degrees");
        fitstable_add_write_column(tab, dubl, "index_x",   "pixels");
        fitstable_add_write_column(tab, dubl, "index_y",   "pixels");
        fitstable_add_write_column(tab, dubl, "index_ra",  "degrees");
        fitstable_add_write_column(tab, dubl, "index_dec", "degrees");
        fitstable_add_write_column(tab, itype, "index_id", "none");
        fitstable_add_write_column(tab, itype, "field_id", "none");
        fitstable_add_write_column(tab, dubl, "match_weight", "none");
		
        if (mo->tagalong) {
            for (j=0; j<bl_size(mo->tagalong); j++) {
                tagalong_t* tag = bl_access(mo->tagalong, j);
                fitstable_add_write_column_struct(tab, tag->type, tag->arraysize, 0, tag->type, tag->name, tag->units);
                tag->colnum = fitstable_ncols(tab)-1;
            }
        }

        // FIXME -- check for duplicate column names
        if (mo->field_tagalong) {
            int j;
            for (j=0; j<bl_size(mo->field_tagalong); j++) {
                tagalong_t* tag = bl_access(mo->field_tagalong, j);
                fitstable_add_write_column_struct(tab, tag->type, tag->arraysize, 0, tag->type, tag->name, tag->units);
                tag->colnum = fitstable_ncols(tab)-1;
            }
        }

        if (fitstable_write_header(tab)) {
            ERROR("Failed to write correspondence file header.");
            return -1;
        }

        {
            int rows = 0;
            for (j=0; j<mo->nfield; j++) {
                if (mo->theta[j] < 0)
                    continue;
                rows++;
            }
            logverb("Writing %i rows (of %i field and %i index objects) to correspondence file.\n", rows, mo->nfield, mo->nindex);
        }
        for (j=0; j<mo->nfield; j++) {
            double fx,fy,fra,fdec;
            double rx,ry,rra,rdec;
            double weight;
            int ti, ri;
            ri = mo->theta[j];
            if (ri < 0)
                continue;
            ti = j;
            rra  = mo->refradec[2*ri+0];
            rdec = mo->refradec[2*ri+1];
            if (!sip_radec2pixelxy(wcs, rra, rdec, &rx, &ry))
                continue;
            fx = mo->fieldxy[2*ti+0];
            fy = mo->fieldxy[2*ti+1];
            sip_pixelxy2radec(wcs, fx, fy, &fra, &fdec);
            logdebug("Writing field xy %.1f,%.1f, radec %.2f,%.2f; index xy %.1f,%.1f, radec %.2f,%.2f\n", fx, fy, fra, fdec, rx, ry, rra, rdec);
            weight = verify_logodds_to_weight(mo->matchodds[j]);
            if (fitstable_write_row(tab, &fx, &fy, &fra, &fdec, &rx, &ry, &rra, &rdec, &ri, &ti, &weight)) {
                ERROR("Failed to write coordinates to correspondences file \"%s\"", bp->corr_fname);
                return -1;
            }
        }

        if (mo->tagalong) {
            for (j=0; j<bl_size(mo->tagalong); j++) {
                tagalong_t* tag = bl_access(mo->tagalong, j);
                int row = 0;
                int k;
                // Ugh, we write each datum individually...
                for (k=0; k<mo->nfield; k++) {
                    int ri = mo->theta[k];
                    if (ri < 0)
                        continue;
                    fitstable_write_one_column(tab, tag->colnum, row, 1,
                                               (char*)tag->data + ri*tag->itemsize, 0);
                    row++;
                }
            }
        }
        if (mo->field_tagalong) {
            for (j=0; j<bl_size(mo->field_tagalong); j++) {
                tagalong_t* tag = bl_access(mo->field_tagalong, j);
                int row = 0;
                int k;
                // Ugh, we write each datum individually...
                for (k=0; k<mo->nfield; k++) {
                    if (mo->theta[k] < 0)
                        continue;
                    fitstable_write_one_column(tab, tag->colnum, row, 1,
                                               (char*)tag->data + k*tag->itemsize, 0);
                    row++;
                }
            }
        }
		
        if (fitstable_fix_header(tab)) {
            ERROR("Failed to fix correspondence file header.");
            return -1;
        }

        fitstable_next_extension(tab);
        fitstable_clear_table(tab);
    }

    if (fitstable_close(tab)) {
        ERROR("Failed to close correspondence file");
        return -1;
    }

    return 0;
}

static int write_solutions(onefield_t* bp) {
    anbool got_solutions = (bl_size(bp->solutions) > 0);

    // If we found no solution, don't write empty output files!
    if (!got_solutions)
        return 0;

    // The solutions can fall out of order because tweak2() updates their logodds.
    bl_sort(bp->solutions, compare_matchobjs);

    if (bp->matchfname) {
        if (write_match_file(bp))
            return -1;
    }
    if (bp->indexrdlsfname) {
        if (write_rdls_file(bp))
            return -1;
    }

    // We only want the best solution for each field in the following outputs:
    remove_duplicate_solutions(bp);

    if (bp->wcs_template) {
        if (write_wcs_file(bp))
            return -1;
    }
    if (bp->scamp_fname) {
        if (write_scamp_file(bp))
            return -1;
    }
    if (bp->corr_fname) {
        if (write_corr_file(bp))
            return -1;
    }
    return 0;
}

static int compare_matchobjs(const void* v1, const void* v2) {
    int diff;
    float fdiff;
    const MatchObj* mo1 = v1;
    const MatchObj* mo2 = v2;
    diff = mo1->fieldfile - mo2->fieldfile;
    if (diff) return diff;
    diff = mo1->fieldnum - mo2->fieldnum;
    if (diff) return diff;
    fdiff = mo1->logodds - mo2->logodds;
    if (fdiff == 0.0)
        return 0;
    if (fdiff > 0.0)
        return -1;
    return 1;
}
