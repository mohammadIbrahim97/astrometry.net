/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */

#include <assert.h>
#include <libgen.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "bl-sort.h"
#include "boilerplate.h"
#include "errors.h"
#include "fitsioutils.h"
#include "index.h"
#include "ioutils.h"
#include "log.h"
#include "mathutil.h"
#include "onefield_internal.h"
#include "solver_field_geometry_internal.h"
#include "os-features.h"
#include "permutedsort.h"
#include "tic.h"
#include "verify.h"

typedef struct onefield_index_shard_worker_view {
  solver_t solver;
  struct stat source_identity;
  void *index_snapshot_storage;
  const char **index_names;
  char *index_name_arena;
  index_t **loaded_indexes;
  size_t index_name_count;
  size_t loaded_index_count;
  int index_options;
  /* Borrowed from the master until every worker has quiesced. */
  char *fieldfname;
  char *indexrdlsfname;
  char *corr_fname;
  char *scamp_fname;
  char *solved_in;
  char *xcolname;
  char *ycolname;
  char *fieldid_key;
  char *sort_rdls;
  char *cancelfname;
  double logratio_tosolve;
  int nsolves;
  int fieldnum;
  int fieldid;
} onefield_index_shard_worker_view_t;

static index_shard_hook_result_t onefield_index_shard_hook_result(
    index_shard_hook_outcome_t outcome,
    int error_code) {
  index_shard_hook_result_t result = {outcome, error_code};

  return result;
}

static index_shard_hook_result_t onefield_index_shard_get_index(
    const void *opaque,
    size_t index_order,
    index_t **index_out) {
  const onefield_index_shard_worker_view_t *view = opaque;
  index_t *index;

  if (index_out) {
    *index_out = NULL;
  }
  if (!view || !index_out) {
    return onefield_index_shard_hook_result(
        INDEX_SHARD_HOOK_GLOBAL_INTEGRITY_FAILURE,
        -1);
  }

  if (index_order < view->index_name_count) {
    const char *index_name = view->index_names[index_order];

    index = index_load(index_name, view->index_options, NULL);
    if (!index) {
      ERROR("Failed to load index %s", index_name);
      return onefield_index_shard_hook_result(
          INDEX_SHARD_HOOK_TASK_LOCAL_FAILURE,
          -1);
    }

    *index_out = index;
    return onefield_index_shard_hook_result(
        INDEX_SHARD_HOOK_COMPLETED_UNSOLVED,
        0);
  }

  index_order -= view->index_name_count;
  if (index_order >= view->loaded_index_count) {
    ERROR("Index order %zu is outside the loaded index list", index_order);
    return onefield_index_shard_hook_result(
        INDEX_SHARD_HOOK_GLOBAL_INTEGRITY_FAILURE,
        -1);
  }

  index = view->loaded_indexes[index_order];
  if (!index) {
    return onefield_index_shard_hook_result(
        INDEX_SHARD_HOOK_GLOBAL_INTEGRITY_FAILURE,
        -1);
  }

  *index_out = index;
  return onefield_index_shard_hook_result(
      INDEX_SHARD_HOOK_COMPLETED_UNSOLVED,
      0);
}

static index_shard_hook_result_t
onefield_index_shard_done_with_index(
    onefield_t *bp,
    size_t index_order,
    index_t *index) {
  if (!bp || !index) {
    return onefield_index_shard_hook_result(
        INDEX_SHARD_HOOK_GLOBAL_INTEGRITY_FAILURE,
        -1);
  }
  onefield_internal_done_with_index(bp, index_order, index);
  return onefield_index_shard_hook_result(
      INDEX_SHARD_HOOK_COMPLETED_UNSOLVED,
      0);
}

static int onefield_index_shard_report_committed_solution(
    onefield_t *bp,
    size_t index_order,
    int fieldnum,
    double best_logodds) {
  const char *index_name;
  char *index_base;

  if (!bp || fieldnum < 0) {
    logerr("[index-shard] invalid committed-solution metadata\n");
    return -1;
  }

  index_name = onefield_internal_get_index_name(bp, index_order);
  if (!index_name) {
    logerr("[index-shard] committed index order %zu has no filename\n",
           index_order);
    return -1;
  }

  index_base = basename_safe(index_name);
  if (!index_base) {
    SYSERROR("Failed to allocate committed index basename");
    return -1;
  }

  logmsg("Field %i: solved with index %s.\n", fieldnum, index_base);
  logverb("[index-shard] committed-solution index_order=%zu "
          "field=%i best_logodds=%.17g index_path=%s\n",
          index_order,
          fieldnum,
          best_logodds,
          index_name);

  free(index_base);
  return 0;
}

/*
 * Prepare the immutable field representation once on the pass owner. Worker
 * solvers borrow these pointers and release only their private task state.
 */
int onefield_index_shard_prepare_job_field_for_run(
    onefield_t *bp) {
  int fieldnum;

  if (!bp || il_size(bp->fieldlist) != 1) {
    logerr("[index-shard] shared field preparation requires one field\n");
    return -1;
  }

  fieldnum = il_get(bp->fieldlist, 0);
  bp->fieldnum = fieldnum;

  if (onefield_internal_prepare_field_view(bp, fieldnum)) {
    return -1;
  }
  if (!onefield_internal_field_cache_valid(bp)) {
    logverb("[index-shard] shared-field-cache state=unavailable "
            "reason=identity-key\n");
    return 1;
  }
  if (onefield_check_total_limits(bp)) {
    return 0;
  }

  if (bp->index_shard_workers > 1 &&
      !solver_prepare_field_geometry(&bp->solver)) {
    logverb("[index-shard] shared-field-geometry state=unavailable "
            "fallback=native\n");
  }

  logverb("[onefield] shared-field-cache=job-owned field=%i\n",
          fieldnum);

  return 0;
}

static void onefield_index_shard_destroy_worker_view(void *opaque) {
  onefield_index_shard_worker_view_t *view = opaque;

  if (!view) {
    return;
  }
  free(view->index_snapshot_storage);
  free(view);
}

/*
 * Freeze index lookup into contiguous pass-owned storage. The master block
 * lists have mutable last-access shortcuts, so no worker may traverse either
 * list, including through a nominally const accessor.
 */
static int onefield_index_shard_snapshot_indexes(
    onefield_index_shard_worker_view_t *view,
    onefield_t *master) {
  size_t index_name_table_bytes;
  size_t loaded_index_table_bytes;
  size_t snapshot_bytes;
  size_t arena_bytes = 0U;
  size_t arena_offset = 0U;
  size_t i;

  if (!view || !master) {
    return -1;
  }

  view->index_name_count = (size_t)sl_size(master->indexnames);
  view->loaded_index_count = (size_t)pl_size(master->indexes);
  view->index_options = master->index_options;

  if (view->index_name_count >
      SIZE_MAX / sizeof(*view->index_names)) {
    return -1;
  }
  if (view->loaded_index_count >
      SIZE_MAX / sizeof(*view->loaded_indexes)) {
    return -1;
  }
  index_name_table_bytes =
      view->index_name_count * sizeof(*view->index_names);
  loaded_index_table_bytes =
      view->loaded_index_count * sizeof(*view->loaded_indexes);

  for (i = 0U; i < view->index_name_count; i++) {
    const char *name = sl_get(master->indexnames, i);
    size_t name_bytes;

    if (!name) {
      return -1;
    }
    name_bytes = strlen(name);
    if (name_bytes == SIZE_MAX ||
        arena_bytes > SIZE_MAX - name_bytes - 1U) {
      return -1;
    }
    arena_bytes += name_bytes + 1U;
  }

  if (index_name_table_bytes >
      SIZE_MAX - loaded_index_table_bytes) {
    return -1;
  }
  snapshot_bytes =
      index_name_table_bytes + loaded_index_table_bytes;
  if (snapshot_bytes > SIZE_MAX - arena_bytes) {
    return -1;
  }
  snapshot_bytes += arena_bytes;

  if (snapshot_bytes) {
    char *storage = malloc(snapshot_bytes);

    if (!storage) {
      return -1;
    }
    view->index_snapshot_storage = storage;
    view->index_names = (const char **)storage;
    view->loaded_indexes = (index_t **)(
        storage + index_name_table_bytes);
    view->index_name_arena =
        storage + index_name_table_bytes + loaded_index_table_bytes;
  }

  for (i = 0U; i < view->index_name_count; i++) {
    const char *name = sl_get(master->indexnames, i);
    size_t name_bytes;

    if (!name) {
      return -1;
    }
    name_bytes = strlen(name) + 1U;
    if (arena_offset > arena_bytes ||
        name_bytes > arena_bytes - arena_offset) {
      return -1;
    }

    memcpy(
        view->index_name_arena + arena_offset,
        name,
        name_bytes);
    view->index_names[i] =
        view->index_name_arena + arena_offset;
    arena_offset += name_bytes;
  }
  if (arena_offset != arena_bytes) {
    return -1;
  }

  for (i = 0U; i < view->loaded_index_count; i++) {
    index_t *index = pl_get(master->indexes, i);

    if (!index) {
      return -1;
    }
    view->loaded_indexes[i] = index;
  }

  return 0;
}

/*
 * Initialize one worker from an allowlist of immutable configuration and
 * pass-bounded field views. Mutable solver state, output ownership, open file
 * handles, and task results are deliberately not cloned from the master.
 */
static void onefield_index_shard_initialize_local_solver(
    solver_t *local,
    const solver_t *base) {
  memset(local, 0, sizeof(*local));

  local->fieldxy = base->fieldxy;
  local->pixel_xscale = base->pixel_xscale;
  local->predistort = base->predistort;
  local->fieldxy_orig = base->fieldxy_orig;
  local->funits_lower = base->funits_lower;
  local->funits_upper = base->funits_upper;
  local->logratio_toprint = base->logratio_toprint;
  local->logratio_tokeep = base->logratio_tokeep;
  local->logratio_totune = base->logratio_totune;
  local->distance_from_quad_bonus = base->distance_from_quad_bonus;
  local->verify_uniformize = base->verify_uniformize;
  local->verify_dedup = base->verify_dedup;
  local->do_tweak = base->do_tweak;
  local->tweak_aborder = base->tweak_aborder;
  local->tweak_abporder = base->tweak_abporder;
  local->verify_pix = base->verify_pix;
  local->distractor_ratio = base->distractor_ratio;
  local->codetol = base->codetol;
  local->quadsize_min = base->quadsize_min;
  local->quadsize_max = base->quadsize_max;
  local->startobj = base->startobj;
  local->endobj = base->endobj;
  local->parity = base->parity;
  local->use_radec = base->use_radec;
  memcpy(local->centerxyz, base->centerxyz, sizeof(local->centerxyz));
  local->r2 = base->r2;
  local->logratio_bail_threshold = base->logratio_bail_threshold;
  local->logratio_stoplooking = base->logratio_stoplooking;
  local->maxquads = base->maxquads;
  local->maxmatches = base->maxmatches;
  local->set_crpix = base->set_crpix;
  local->set_crpix_center = base->set_crpix_center;
  memcpy(local->crpix, base->crpix, sizeof(local->crpix));

  local->minminAB2 = base->minminAB2;
  local->maxmaxAB2 = base->maxmaxAB2;
  local->rel_index_noise2 = base->rel_index_noise2;
  local->rel_field_noise2 = base->rel_field_noise2;
  local->abscale_low = base->abscale_low;
  local->abscale_high = base->abscale_high;
  local->field_minx = base->field_minx;
  local->field_maxx = base->field_maxx;
  local->field_miny = base->field_miny;
  local->field_maxy = base->field_maxy;
  local->field_diag = base->field_diag;
  local->cxdx_margin = base->cxdx_margin;
  local->vf = base->vf;
  local->field_geometry = base->field_geometry;
  local->field_geometry_owned = FALSE;

  solver_reset_counters(local);
  local->num_meanx_skipped = 0;
  solver_reset_best_match(local);
}

static int onefield_index_shard_create_worker_view(
    onefield_t *master,
    const solver_t *base_solver,
    void **worker_view_out) {
  onefield_index_shard_worker_view_t *view;
  struct stat source_identity;
  int fieldnum;

  if (!worker_view_out) {
    return -1;
  }
  *worker_view_out = NULL;
  if (!master || !base_solver ||
      il_size(master->fieldlist) != 1 ||
      !base_solver->fieldxy_orig ||
      !base_solver->fieldxy ||
      !base_solver->vf ||
      master->rdls_tagalong ||
      master->rdls_tagalong_all ||
      master->xyls_tagalong ||
      !master->xyls_tagalong_all ||
      !master->fieldfname ||
      stat(master->fieldfname, &source_identity)) {
    return -1;
  }
  fieldnum = il_get_const(master->fieldlist, 0);
  if (!onefield_field_cache_key_matches(
          master, fieldnum, &source_identity)) {
    return -1;
  }

  view = calloc(1, sizeof(*view));
  if (!view) {
    return -1;
  }
  if (onefield_index_shard_snapshot_indexes(view, master)) {
    onefield_index_shard_destroy_worker_view(view);
    return -1;
  }
  onefield_index_shard_initialize_local_solver(
      &view->solver, base_solver);
  view->source_identity = source_identity;
  view->logratio_tosolve = master->logratio_tosolve;
  view->nsolves = master->nsolves;
  view->fieldnum = fieldnum;
  view->fieldid = master->fieldid;
  view->fieldfname = master->fieldfname;
  view->indexrdlsfname = master->indexrdlsfname;
  view->corr_fname = master->corr_fname;
  view->scamp_fname = master->scamp_fname;
  view->solved_in = master->solved_in;
  view->xcolname = master->xcolname;
  view->ycolname = master->ycolname;
  view->fieldid_key = master->fieldid_key;
  view->sort_rdls = master->sort_rdls;
  view->cancelfname = master->cancelfname;

  *worker_view_out = view;
  return 0;
}

static void onefield_index_shard_initialize_local_params(
    onefield_t *local,
    const onefield_index_shard_worker_view_t *view) {
  memset(local, 0, sizeof(*local));
  onefield_index_shard_initialize_local_solver(
      &local->solver, &view->solver);

  local->logratio_tosolve = view->logratio_tosolve;
  local->nsolves = view->nsolves;
  local->fieldfname = view->fieldfname;
  local->indexrdlsfname = view->indexrdlsfname;
  local->corr_fname = view->corr_fname;
  local->scamp_fname = view->scamp_fname;
  local->solved_in = view->solved_in;
  local->fieldnum = view->fieldnum;
  local->fieldid = view->fieldid;
  local->xcolname = view->xcolname;
  local->ycolname = view->ycolname;
  local->fieldid_key = view->fieldid_key;
  local->rdls_tagalong = NULL;
  local->rdls_tagalong_all = FALSE;
  local->sort_rdls = view->sort_rdls;
  local->xyls_tagalong = NULL;
  local->xyls_tagalong_all = TRUE;
  local->cancelfname = view->cancelfname;
}

static void onefield_index_shard_release_local_context(
    onefield_t *local_bp) {
  if (!local_bp) {
    return;
  }
  local_bp->solver.mo_template = NULL;
  local_bp->solver.record_match_callback = NULL;
  local_bp->solver.timer_callback = NULL;
  local_bp->solver.userdata = NULL;
  solver_clear_indexes(&local_bp->solver);

  /* Field storage is borrowed from the master for one complete pass. */
  local_bp->solver.fieldxy_orig = NULL;
  local_bp->solver.fieldxy = NULL;
  local_bp->solver.vf = NULL;
  local_bp->solver.field_geometry = NULL;
  local_bp->solver.field_geometry_owned = FALSE;
  solver_cleanup_field(&local_bp->solver);

  if (local_bp->xyls) {
    xylist_close(local_bp->xyls);
    local_bp->xyls = NULL;
  }
  if (local_bp->solver.indexes) {
    pl_free(local_bp->solver.indexes);
    local_bp->solver.indexes = NULL;
  }
  local_bp->solutions = NULL;
}

static int onefield_index_shard_prepare_local_context(onefield_t *local_bp,
                                                      const void *opaque) {
  const onefield_index_shard_worker_view_t *view = opaque;
  struct stat source_stat_after;
  int fieldnum;

  if (!local_bp || !view) {
    return -1;
  }
  onefield_index_shard_initialize_local_params(
      local_bp, view);

  local_bp->solver.indexes = pl_new(1);
  if (!local_bp->solver.indexes) {
    SYSERROR("Failed to allocate worker-local solver index list");
    return -1;
  }

  if (!view->solver.fieldxy_orig ||
      !view->solver.fieldxy ||
      !view->solver.vf) {
    logerr("[index-shard] shared field view is not prepared\n");
    goto fail;
  }

  fieldnum = view->fieldnum;
  if (stat(view->fieldfname, &source_stat_after) ||
      !stat_file_identity_equal(
          &view->source_identity, &source_stat_after)) {
    logerr("[index-shard] worker field source changed before "
           "local view preparation\n");
    goto fail;
  }

  /*
   * fieldxy_orig, fieldxy and vf were copied from the immutable pass view.
   * The master solver retains ownership until every worker is quiescent.
   */
  local_bp->xyls = xylist_open(view->fieldfname);
  if (!local_bp->xyls) {
    ERROR("Failed to open worker-local xylist %s", view->fieldfname);
    goto fail;
  }

  xylist_set_xname(local_bp->xyls, local_bp->xcolname);
  xylist_set_yname(local_bp->xyls, local_bp->ycolname);
  xylist_set_include_flux(local_bp->xyls, FALSE);
  xylist_set_include_background(local_bp->xyls, FALSE);

  if (xylist_open_field(local_bp->xyls, fieldnum)) {
    logerr("Failed to open extension %i in worker-local xylist.\n",
           fieldnum);
    goto fail;
  }
  if (stat(view->fieldfname, &source_stat_after) ||
      !stat_file_identity_equal(
          &view->source_identity, &source_stat_after)) {
    logerr("[index-shard] worker field source changed during "
           "local view preparation\n");
    goto fail;
  }

  logverb("[index-shard] worker-field-view=borrowed field=%i\n",
          fieldnum);

  return 0;

fail:
  onefield_index_shard_release_local_context(local_bp);
  return -1;
}

static void
onefield_index_shard_reset_local_context_for_task(onefield_t *local_bp,
                                                  bl *local_solutions) {
  local_bp->solutions = local_solutions;

  local_bp->single_field_solved = FALSE;
  local_bp->solver_failed = FALSE;
  local_bp->nsolves_sofar = 0;

  local_bp->hit_cpulimit = FALSE;
  local_bp->hit_total_cpulimit = FALSE;
  local_bp->hit_timelimit = FALSE;
  local_bp->hit_total_timelimit = FALSE;
  local_bp->cancelled = FALSE;

  local_bp->solver.quit_now = FALSE;
  local_bp->solver.index = NULL;
  memset(&local_bp->solver.profile,
         0,
         sizeof(local_bp->solver.profile));

  solver_reset_counters(&local_bp->solver);
  local_bp->solver.num_meanx_skipped = 0;
  solver_reset_best_match(&local_bp->solver);

  solver_clear_indexes(&local_bp->solver);
}

/*
 * Run one index against the immutable field representation prepared once by
 * the pass owner. The owner-visible MatchObj template and every solver counter
 * remain task-local; only the XYLS field, star-list copy and verification
 * KD-tree are retained until the outer pass has quiesced.
 */
static int onefield_index_shard_solve_preprocessed_field(onefield_t *local_bp) {
  solver_t *sp = &local_bp->solver;
  MatchObj template;
  qfits_header *fieldhdr;
  anbool interrupted_by_parallel_stop = FALSE;
  int fieldnum = local_bp->fieldnum;

  if (!sp->fieldxy_orig || !sp->fieldxy || !sp->vf) {
    logerr("[index-shard] shared field view is not prepared\n");
    sp->profile.execution_failed = TRUE;
    local_bp->solver_failed = TRUE;
    return -1;
  }

  memset(&template, 0, sizeof(MatchObj));
  template.fieldnum = fieldnum;
  template.fieldfile = local_bp->fieldid;

  fieldhdr = xylist_get_header(local_bp->xyls);
  if (fieldhdr) {
    char *idstr = fits_get_dupstring(fieldhdr, local_bp->fieldid_key);

    if (idstr) {
      strncpy(template.fieldname,
              idstr,
              sizeof(template.fieldname) - 1);
    }
    free(idstr);
  }

  sp->mo_template = &template;
  sp->record_match_callback = onefield_internal_record_match_callback;
  sp->timer_callback = onefield_internal_timer_callback;
  sp->userdata = local_bp;
  sp->distance_from_quad_bonus = TRUE;

  local_bp->nsolves_sofar = 0;
  logverb("Solving field %i.\n", fieldnum);
  solver_log_params(sp);

  if (solver_run(sp)) {
    local_bp->solver_failed = TRUE;
  }

  /*
   * A losing owner can observe the pass stop while unwinding from solver_run.
   * Its partial traversal is not a completed scientific "did not solve"
   * result. Preserve the ordinary per-index report for completed work and
   * local user/limit cancellation, but omit this one misleading observation.
   */
  interrupted_by_parallel_stop =
      index_shard_worker_stop_requested() &&
      !local_bp->cancelled &&
      !local_bp->hit_total_timelimit &&
      !local_bp->hit_total_cpulimit;

  sp->mo_template = NULL;
  sp->record_match_callback = NULL;
  sp->timer_callback = NULL;
  sp->userdata = NULL;

  if (local_bp->solver_failed || sp->profile.execution_failed) {
    local_bp->solver_failed = TRUE;
    logerr("Solver execution failed for field %i\n", fieldnum);
    return -1;
  }

  logverb("Field %i: tried %i quads, matched %i codes.\n",
          fieldnum,
          sp->numtries,
          sp->nummatches);

  if (sp->maxquads && sp->numtries >= sp->maxquads) {
    logmsg("  exceeded the number of quads to try: %i >= %i.\n",
           sp->numtries,
           sp->maxquads);
  }
  if (sp->maxmatches && sp->nummatches >= sp->maxmatches) {
    logmsg("  exceeded the number of quads to match: %i >= %i.\n",
           sp->nummatches,
           sp->maxmatches);
  }
  if (local_bp->cancelled) {
    logmsg("  cancelled at user request.\n");
  }

  if (sp->best_match_solves) {
    local_bp->single_field_solved = TRUE;
  } else if (!interrupted_by_parallel_stop &&
             sp->index && sp->index->indexname) {
    char *copy = strdup_safe(sp->index->indexname);
    char *base = basename(copy);

    if (sp->endobj) {
      logerr("Field %i did not solve (index %s, field objects %i-%i).\n",
             fieldnum,
             base,
             sp->startobj + 1,
             sp->endobj);
    } else {
      logerr("Field %i did not solve (index %s).\n",
             fieldnum,
             base);
    }
    free(copy);

    if (sp->have_best_match) {
      logverb("Best match encountered: ");
      matchobj_print(&sp->best_match, log_get_level());
    } else {
      logverb("Best odds encountered: %g\n", exp(sp->best_logodds));
    }
  } else if (!interrupted_by_parallel_stop) {
    logerr("Field %i did not solve.\n", fieldnum);
  }

  return 0;
}

static index_shard_hook_result_t
onefield_index_shard_solve_one_index(
    onefield_t *local_bp,
    index_t *index) {
  index_shard_hook_result_t hook_result;
  int rc;

  if (!local_bp || !index) {
    return onefield_index_shard_hook_result(
        INDEX_SHARD_HOOK_GLOBAL_INTEGRITY_FAILURE,
        -1);
  }

  solver_add_index(&local_bp->solver, index);

  local_bp->cpu_start = get_cpu_usage();
  local_bp->time_start = monotonic_seconds();

  rc = onefield_index_shard_solve_preprocessed_field(local_bp);
  if (rc || local_bp->solver_failed ||
      local_bp->solver.profile.execution_failed) {
    hook_result = onefield_index_shard_hook_result(
        INDEX_SHARD_HOOK_GLOBAL_INTEGRITY_FAILURE,
        rc ? rc : -1);
  } else if (local_bp->cancelled) {
    hook_result = onefield_index_shard_hook_result(
        INDEX_SHARD_HOOK_CANCELLED,
        0);
  } else if (local_bp->hit_total_timelimit) {
    hook_result = onefield_index_shard_hook_result(
        INDEX_SHARD_HOOK_WALL_LIMIT,
        0);
  } else if (local_bp->hit_total_cpulimit) {
    hook_result = onefield_index_shard_hook_result(
        INDEX_SHARD_HOOK_CPU_LIMIT,
        0);
  } else {
    hook_result = onefield_index_shard_hook_result(
        INDEX_SHARD_HOOK_COMPLETED_UNSOLVED,
        0);
  }

  solver_clear_indexes(&local_bp->solver);
  return hook_result;
}

static index_shard_hook_result_t
onefield_index_shard_analyze_solutions(
    onefield_t *master_bp,
    bl *solutions,
    double *best_logodds,
    int *best_fieldnum) {
  int required_solutions;
  int solution_count = 0;
  int i;

  if (best_logodds) {
    *best_logodds = -HUGE_VAL;
  }

  if (best_fieldnum) {
    *best_fieldnum = -1;
  }

  if (!master_bp || !solutions) {
    return onefield_index_shard_hook_result(
        INDEX_SHARD_HOOK_GLOBAL_INTEGRITY_FAILURE,
        -1);
  }

  if (il_size(master_bp->fieldlist) != 1) {
    logerr("[index-shard] refusing non-single-field solution analysis\n");
    return onefield_index_shard_hook_result(
        INDEX_SHARD_HOOK_GLOBAL_INTEGRITY_FAILURE,
        -1);
  }

  required_solutions = MAX(1, master_bp->nsolves);

  for (i = 0; i < bl_size(solutions); i++) {
    MatchObj *mo = bl_access(solutions, i);

    if (mo->logodds >= master_bp->logratio_tosolve) {
      solution_count++;
    }

    if (best_logodds && mo->logodds > *best_logodds) {
      *best_logodds = mo->logodds;

      if (best_fieldnum) {
        *best_fieldnum = mo->fieldnum;
      }
    }
  }

  return onefield_index_shard_hook_result(
      solution_count >= required_solutions
          ? INDEX_SHARD_HOOK_COMPLETED_SOLVED
          : INDEX_SHARD_HOOK_COMPLETED_UNSOLVED,
      0);
}

static void onefield_index_shard_disown_matchobj(MatchObj *mo) {
  if (!mo) {
    return;
  }

  mo->sip = NULL;
  mo->refradec = NULL;
  mo->fieldxy = NULL;
  mo->theta = NULL;
  mo->matchodds = NULL;
  mo->refxyz = NULL;
  mo->refxy = NULL;
  mo->refstarid = NULL;
  mo->testperm = NULL;
  mo->tagalong = NULL;
  mo->field_tagalong = NULL;
}

static int onefield_index_shard_merge_solutions(onefield_t *master_bp,
                                                bl *solutions,
                                                anbool *solved_out) {
  int required_solutions;
  int solution_count = 0;
  int i;
  anbool solved = FALSE;

  if (solved_out) {
    *solved_out = FALSE;
  }

  if (!master_bp || !solutions) {
    return 0;
  }

  if (il_size(master_bp->fieldlist) != 1) {
    logerr("[index-shard] refusing non-single-field solution merge\n");
    return -1;
  }

  required_solutions = MAX(1, master_bp->nsolves);

  for (i = 0; i < bl_size(solutions); i++) {
    MatchObj *src = bl_access(solutions, i);

    bl_insert_sorted(master_bp->solutions, src, onefield_internal_compare_matchobjs);

    if (src->logodds >= master_bp->logratio_tosolve) {
      solution_count++;
    }

    if (solution_count == required_solutions) {
      /*
       * The serial callback declares a field solved on exactly the Nth
       * above-threshold match. Preserve that nsolves contract per field at the
       * authoritative master commit point. The worker already emitted the
       * chronological MatchObj diagnostics; do not print a sorted-list entry
       * here and misrepresent it as the Nth chronological hit.
       */
      onefield_internal_solved_field(master_bp, src->fieldnum);
      solved = TRUE;
    }

    onefield_index_shard_disown_matchobj(src);
  }

  bl_remove_all(solutions);

  if (solved_out) {
    *solved_out = solved;
  }

  return 0;
}

static void onefield_index_shard_free_solutions(bl *solutions) {
  int i;

  if (!solutions) {
    return;
  }

  for (i = 0; i < bl_size(solutions); i++) {
    MatchObj *mo = bl_access(solutions, i);
    verify_free_matchobj(mo);
    onefield_free_matchobj(mo);
  }

  bl_free(solutions);
}

static const index_shard_hooks_t onefield_index_shard_hooks = {
    .get_index = onefield_index_shard_get_index,
    .done_with_index = onefield_index_shard_done_with_index,
    .report_committed_solution =
        onefield_index_shard_report_committed_solution,

    .create_worker_view =
        onefield_index_shard_create_worker_view,
    .destroy_worker_view =
        onefield_index_shard_destroy_worker_view,
    .prepare_local_context =
        onefield_index_shard_prepare_local_context,
    .reset_local_context_for_task =
        onefield_index_shard_reset_local_context_for_task,
    .cleanup_local_context =
        onefield_index_shard_release_local_context,

    .solve_one_index = onefield_index_shard_solve_one_index,
    .analyze_solutions =
        onefield_index_shard_analyze_solutions,
    .merge_solutions = onefield_index_shard_merge_solutions,
    .free_solutions = onefield_index_shard_free_solutions};

index_shard_solve_status_t onefield_index_shard_solve(
    onefield_t* bp,
    solver_t* solver,
    size_t index_count) {
  return index_shard_solve(
      bp,
      solver,
      index_count,
      &onefield_index_shard_hooks);
}
