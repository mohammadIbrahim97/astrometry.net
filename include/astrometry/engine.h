/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */

#ifndef ENGINE_H
#define ENGINE_H

#include <stdio.h>

#include "astrometry/onefield.h"
#include "astrometry/bl.h"
#include "astrometry/an-bool.h"
#include "astrometry/index.h"

struct index_shard_pool;

struct engine {
    // search paths (directories)
    sl* index_paths;

    // Contains metadata-only ordinary indexes. Multiindex components retain
    // their separately owned shared StarKD handle.
    pl* indexes;

    // indexes that need to be freed
    pl* free_indexes;
    // multiindexes that need to be freed
    pl* free_mindexes;

    il* ibiggest;
    il* ismallest;
    il* default_depths;
    double sizesmallest;
    double sizebiggest;
    anbool inparallel;
    double minwidth;
    double maxwidth;
    double walllimit;
    float cpulimit;
    /*
     * Site-level worker request. Zero is the internal representation of
     * textual "auto"; positive values are explicit counts.
     */
    int index_shard_workers_config;
    anbool index_shard_workers_config_set;
    /*
     * Process-lifetime parallel service.  Job and pass pointers are bound
     * only while engine_run_job() owns the corresponding onefield state.
     */
    struct index_shard_pool* index_shard_pool;
    char* cancelfn;
    char* solvedfn;
};
typedef struct engine engine_t;

struct job_t {
    dl* scales;
    il* depths;
    anbool include_default_scales;
    double ra_center;
    double dec_center;
    double search_radius;
    anbool use_radec_center;
    int index_shard_workers_override;
    anbool index_shard_workers_override_set;
    anbool index_shard_workers_controlled;
    onefield_t bp;
};
typedef struct job_t job_t;

typedef enum engine_job_outcome {
    ENGINE_JOB_OUTCOME_UNSOLVED = 0,
    ENGINE_JOB_OUTCOME_SOLVED,
    ENGINE_JOB_OUTCOME_WALL_LIMIT,
    ENGINE_JOB_OUTCOME_CPU_LIMIT,
    ENGINE_JOB_OUTCOME_CANCELLED,
    ENGINE_JOB_OUTCOME_ERROR
} engine_job_outcome_t;

/*
 * Pointer-free snapshot of one completed engine job.  The stop flags retain
 * overlapping observations while outcome applies the authoritative terminal
 * precedence used by the streamed protocol.
 */
typedef struct engine_job_result {
    int engine_rc;
    engine_job_outcome_t outcome;
    anbool solved;
    anbool cancelled;
    anbool wall_limit;
    anbool cpu_limit;
    anbool execution_error;
} engine_job_result_t;

engine_t* engine_new();
void engine_add_search_path(engine_t* engine, const char* path);
char* engine_find_index(engine_t*, const char* name);
// note that "path" must be a full path name.
int engine_add_index(engine_t* engine, char* path);
// look in all the search path directories for index files.
int engine_autoindex_search_paths(engine_t* engine);
int engine_parse_config_file_stream(engine_t* engine, FILE* fconf);
int engine_parse_config_file(engine_t* engine, const char* fn);
int engine_run_job(engine_t* engine, job_t* job);
int engine_run_job_with_result(engine_t* engine, job_t* job,
                               engine_job_result_t* result);
const char* engine_job_outcome_string(engine_job_outcome_t outcome);
void engine_free(engine_t* engine);

job_t* engine_read_job_file(engine_t* engine, const char* jobfn);
int job_set_base_dir(job_t* job, const char* dir);
int job_set_input_base_dir(job_t* job, const char* dir);
int job_set_output_base_dir(job_t* job, const char* dir);
void job_set_cancel_file(job_t* job, const char* fn);
void job_set_solved_file(job_t* job, const char* fn);
void job_free(job_t* job);


#endif
