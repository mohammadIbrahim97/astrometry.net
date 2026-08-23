/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "errors.h"
#include "index.h"
#include "ioutils.h"
#include "log.h"
#include "onefield_internal.h"
#include "solver_field_geometry_internal.h"

struct onefield_job_field_cache {
    anbool valid;
    int fieldnum;
    struct stat source_stat;
    char* xcolname;
    char* ycolname;
    double pixel_xscale;
    const sip_t* predistort;
    anbool verify_uniformize;
    anbool verify_dedup;
    anbool set_crpix;
    anbool set_crpix_center;
    double crpix[2];
    double field_minx;
    double field_maxx;
    double field_miny;
    double field_maxy;
};

size_t onefield_internal_index_count(const onefield_t* bp) {
    return sl_size(bp->indexnames) + pl_size(bp->indexes);
}

index_t* onefield_internal_get_index(onefield_t* bp, size_t index_order) {
    if (index_order < sl_size(bp->indexnames)) {
        char* fn = sl_get(bp->indexnames, index_order);
        index_t* ind = index_load(
            fn,
            bp->index_options,
            NULL);
        if (!ind) {
            ERROR("Failed to load index %s", fn);
            exit(-1);
        }
        return ind;
    }
    index_order -= sl_size(bp->indexnames);
    return pl_get(bp->indexes, index_order);
}

char* onefield_internal_get_index_name(onefield_t* bp,
                                       size_t index_order) {
    index_t* index;
    if (index_order < sl_size(bp->indexnames)) {
        char* fn = sl_get(bp->indexnames, index_order);
        return fn;
    }
    index_order -= sl_size(bp->indexnames);
    index = pl_get(bp->indexes, index_order);
    return index->indexname;
}

void onefield_internal_done_with_index(onefield_t* bp,
                                       size_t index_order,
                                       index_t* index) {
    if (index_order < sl_size(bp->indexnames)) {
        index_free(index);
    }
}

static void onefield_field_cache_clear_key(
    onefield_job_field_cache_t* cache) {
    if (!cache) {
        return;
    }
    free(cache->xcolname);
    free(cache->ycolname);
    cache->xcolname = NULL;
    cache->ycolname = NULL;
    cache->valid = FALSE;
}

int onefield_job_field_cache_begin(onefield_t* bp) {
    if (!bp) {
        return -1;
    }
    if (bp->job_field_cache) {
        return 0;
    }
    bp->job_field_cache =
        calloc(1, sizeof(*bp->job_field_cache));
    if (!bp->job_field_cache) {
        /*
         * Retention is optional. Callers may continue through the exact
         * per-run field lifecycle if metadata allocation is unavailable.
         */
        logverb("[index-shard] job-field-cache state=disabled "
                "reason=allocation\n");
        return 0;
    }
    logverb("[index-shard] job-field-cache state=begin\n");
    return 0;
}

void onefield_job_field_cache_invalidate(onefield_t* bp) {
    if (!bp) {
        return;
    }
    solver_cleanup_field(&bp->solver);
    if (bp->job_field_cache) {
        onefield_field_cache_clear_key(bp->job_field_cache);
    }
    if (bp->xyls) {
        xylist_close(bp->xyls);
        bp->xyls = NULL;
    }
}

void onefield_job_field_cache_end(onefield_t* bp) {
    if (!bp || !bp->job_field_cache) {
        return;
    }
    logverb("[index-shard] job-field-cache state=end\n");
    onefield_job_field_cache_invalidate(bp);
    free(bp->job_field_cache);
    bp->job_field_cache = NULL;
}

int onefield_internal_open_master_xyls(onefield_t* bp) {
    if (!bp || !bp->fieldfname) {
        return -1;
    }
    if (bp->xyls) {
        return 0;
    }

    logverb("Reading fields file %s...", bp->fieldfname);
    bp->xyls = xylist_open(bp->fieldfname);
    if (!bp->xyls) {
        ERROR("Failed to read xylist.\n");
        return -1;
    }
    xylist_set_xname(bp->xyls, bp->xcolname);
    xylist_set_yname(bp->xyls, bp->ycolname);
    xylist_set_include_flux(bp->xyls, FALSE);
    xylist_set_include_background(bp->xyls, FALSE);
    logverb("found %u fields.\n", xylist_n_fields(bp->xyls));
    return 0;
}

int onefield_internal_validate_single_field_list(onefield_t* bp) {
    int acquisition_attempt;

    if (!bp || il_size(bp->fieldlist) != 1) {
        return 0;
    }
    for (acquisition_attempt = 1;
         acquisition_attempt <= 2;
         acquisition_attempt++) {
        struct stat source_stat;
        struct stat source_stat_after;
        xylist_t* probe;
        int field_count;

        if (stat(bp->fieldfname, &source_stat)) {
            if (acquisition_attempt < 2) {
                continue;
            }
            logerr("Failed to identify XYLS source %s.\n",
                   bp->fieldfname);
            return -1;
        }
        probe = xylist_open(bp->fieldfname);
        if (!probe) {
            if (acquisition_attempt < 2) {
                continue;
            }
            logerr("Failed to inspect XYLS source %s.\n",
                   bp->fieldfname);
            return -1;
        }
        field_count = xylist_n_fields(probe);
        xylist_close(probe);
        if (stat(bp->fieldfname, &source_stat_after) ||
            !stat_file_identity_equal(
                &source_stat,
                &source_stat_after)) {
            if (acquisition_attempt < 2) {
                continue;
            }
            logerr("XYLS source changed during both field-list "
                   "validation attempts.\n");
            return -1;
        }
        onefield_internal_remove_invalid_fields(bp->fieldlist, field_count);
        return 0;
    }
    return -1;
}

anbool onefield_field_cache_key_matches(
    const onefield_t* bp,
    int fieldnum,
    const struct stat* source_stat) {
    const onefield_job_field_cache_t* cache;
    const solver_t* sp;
    if (!bp || !source_stat || !bp->job_field_cache) {
        return FALSE;
    }
    cache = bp->job_field_cache;
    sp = &bp->solver;
    if (!cache->valid || !cache->xcolname || !cache->ycolname) {
        return FALSE;
    }
    return cache->fieldnum == fieldnum &&
        stat_file_identity_equal(
            &cache->source_stat, source_stat) &&
        !strcmp(cache->xcolname,
                bp->xcolname ? bp->xcolname : "") &&
        !strcmp(cache->ycolname,
                bp->ycolname ? bp->ycolname : "") &&
        cache->pixel_xscale == sp->pixel_xscale &&
        cache->predistort == sp->predistort &&
        cache->verify_uniformize == sp->verify_uniformize &&
        cache->verify_dedup == sp->verify_dedup &&
        cache->set_crpix == sp->set_crpix &&
        cache->set_crpix_center == sp->set_crpix_center &&
        cache->crpix[0] == sp->crpix[0] &&
        cache->crpix[1] == sp->crpix[1] &&
        cache->field_minx == sp->field_minx &&
        cache->field_maxx == sp->field_maxx &&
        cache->field_miny == sp->field_miny &&
        cache->field_maxy == sp->field_maxy;
}

static anbool onefield_field_cache_record_key(
    onefield_t* bp,
    int fieldnum,
    const struct stat* source_stat) {
    onefield_job_field_cache_t* cache = bp->job_field_cache;
    solver_t* sp = &bp->solver;
    char* xcolname;
    char* ycolname;

    if (!cache || !source_stat) {
        return FALSE;
    }
    xcolname = strdup(bp->xcolname ? bp->xcolname : "");
    ycolname = strdup(bp->ycolname ? bp->ycolname : "");
    if (!xcolname || !ycolname) {
        free(xcolname);
        free(ycolname);
        onefield_field_cache_clear_key(cache);
        logverb("[index-shard] job-field-cache state=disabled "
                "reason=key-allocation\n");
        return FALSE;
    }
    onefield_field_cache_clear_key(cache);
    cache->xcolname = xcolname;
    cache->ycolname = ycolname;
    cache->fieldnum = fieldnum;
    cache->source_stat = *source_stat;
    cache->pixel_xscale = sp->pixel_xscale;
    cache->predistort = sp->predistort;
    cache->verify_uniformize = sp->verify_uniformize;
    cache->verify_dedup = sp->verify_dedup;
    cache->set_crpix = sp->set_crpix;
    cache->set_crpix_center = sp->set_crpix_center;
    cache->crpix[0] = sp->crpix[0];
    cache->crpix[1] = sp->crpix[1];
    cache->field_minx = sp->field_minx;
    cache->field_maxx = sp->field_maxx;
    cache->field_miny = sp->field_miny;
    cache->field_maxy = sp->field_maxy;
    cache->valid = TRUE;
    return TRUE;
}

void onefield_internal_reset_field_pass_state(onefield_t* bp) {
    solver_t* sp;

    if (!bp) {
        return;
    }
    sp = &bp->solver;
    solver_reset_best_match(sp);
    solver_reset_counters(sp);
    sp->index = NULL;
    sp->mo_template = NULL;
    sp->record_match_callback = NULL;
    sp->timer_callback = NULL;
    sp->userdata = NULL;
    memset(&sp->profile, 0, sizeof(sp->profile));
}

int onefield_internal_prepare_field_view(
    onefield_t* bp,
    int fieldnum) {
    onefield_job_field_cache_t* cache;
    solver_t* sp;
    int acquisition_attempt;

    if (!bp) {
        return -1;
    }
    sp = &bp->solver;
    cache = bp->job_field_cache;

    /*
     * qfits table reads reopen the source pathname. Bracket the complete
     * metadata/column/preprocess acquisition and retry once from a freshly
     * opened XYLS object if the pathname identity changes. No cache key is
     * published until the closing stat matches the opening stat.
     */
    for (acquisition_attempt = 1;
         acquisition_attempt <= 2;
         acquisition_attempt++) {
        struct stat source_stat;
        anbool retainable =
            cache && il_size(bp->fieldlist) == 1;
        anbool source_stat_valid = FALSE;

        if (!stat(bp->fieldfname, &source_stat)) {
            source_stat_valid = TRUE;
        } else if (retainable) {
            logverb("[onefield] job-field-cache state=retry "
                    "reason=source-identity field=%i attempt=%i\n",
                    fieldnum,
                    acquisition_attempt);
            onefield_job_field_cache_invalidate(bp);
            if (acquisition_attempt < 2) {
                continue;
            }
            logerr("Failed to identify XYLS source for field %i "
                   "during both acquisition attempts.\n",
                   fieldnum);
            return -1;
        }

        if (retainable &&
            onefield_field_cache_key_matches(
                bp, fieldnum, &source_stat) &&
            sp->fieldxy_orig && sp->fieldxy && sp->vf) {
            struct stat source_stat_after;

            if (onefield_internal_open_master_xyls(bp) ||
                xylist_open_field(bp->xyls, fieldnum)) {
                logerr("Failed to reopen extension %i in xylist.\n",
                       fieldnum);
                onefield_job_field_cache_invalidate(bp);
                if (acquisition_attempt < 2) {
                    continue;
                }
                return -1;
            }
            if (stat(bp->fieldfname, &source_stat_after) ||
                !stat_file_identity_equal(
                    &source_stat,
                    &source_stat_after)) {
                logverb("[index-shard] job-field-cache state=retry "
                        "reason=source-changed-during-hit field=%i "
                        "attempt=%i\n",
                        fieldnum,
                        acquisition_attempt);
                onefield_job_field_cache_invalidate(bp);
                if (acquisition_attempt < 2) {
                    continue;
                }
                logerr("XYLS source changed during both acquisition "
                       "attempts for field %i.\n",
                       fieldnum);
                return -1;
            }
            solver_release_incompatible_field_geometry(sp);
            onefield_internal_reset_field_pass_state(bp);
            logverb("[index-shard] job-field-cache state=hit field=%i\n",
                    fieldnum);
            return 0;
        }

        if (cache && cache->valid) {
            logverb("[index-shard] job-field-cache state=invalidate "
                    "reason=identity-or-preprocess-key\n");
        }
        onefield_job_field_cache_invalidate(bp);
        if (onefield_internal_open_master_xyls(bp)) {
            if (acquisition_attempt < 2) {
                continue;
            }
            return -1;
        }
        if (xylist_open_field(bp->xyls, fieldnum)) {
            logerr("Failed to open extension %i in xylist.\n",
                   fieldnum);
            onefield_job_field_cache_invalidate(bp);
            if (acquisition_attempt < 2) {
                continue;
            }
            return -1;
        }
        solver_set_field(sp, xylist_read_field(bp->xyls, NULL));
        if (!sp->fieldxy_orig) {
            logerr("Failed to read xylist field.\n");
            onefield_job_field_cache_invalidate(bp);
            if (acquisition_attempt < 2) {
                continue;
            }
            return -1;
        }
        solver_preprocess_field(sp);
        if (!sp->fieldxy || !sp->vf) {
            logerr("Failed to preprocess xylist field.\n");
            onefield_job_field_cache_invalidate(bp);
            return -1;
        }
        solver_release_incompatible_field_geometry(sp);
        onefield_internal_reset_field_pass_state(bp);
        if (source_stat_valid) {
            struct stat source_stat_after;

            if (stat(bp->fieldfname, &source_stat_after) ||
                !stat_file_identity_equal(
                    &source_stat,
                    &source_stat_after)) {
                logverb("[index-shard] job-field-cache state=retry "
                    "reason=source-changed-during-fill field=%i\n",
                    fieldnum);
                onefield_job_field_cache_invalidate(bp);
                if (acquisition_attempt < 2) {
                    continue;
                }
                logerr("XYLS source changed during both acquisition "
                       "attempts for field %i.\n",
                       fieldnum);
                return -1;
            }
            if (retainable) {
                if (!onefield_field_cache_record_key(
                        bp, fieldnum, &source_stat_after)) {
                    retainable = FALSE;
                }
            }
        }
        logverb("[index-shard] job-field-cache state=fill field=%i "
                "retained=%i\n",
                fieldnum,
                retainable ? 1 : 0);
        return 0;
    }
    return -1;
}

anbool onefield_internal_field_cache_valid(const onefield_t* bp) {
    return bp && bp->job_field_cache &&
        bp->job_field_cache->valid;
}

anbool onefield_internal_field_cache_has_field(const onefield_t* bp,
                                               int fieldnum) {
    return onefield_internal_field_cache_valid(bp) &&
        bp->job_field_cache->fieldnum == fieldnum;
}
