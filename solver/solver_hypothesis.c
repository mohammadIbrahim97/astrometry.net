/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */
/* AB-pair enumeration, descriptors, and bounded verification waves. */

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "solver.h"
#include "verify.h"
#include "fit-wcs.h"
#include "fitsbin.h"
#include "log.h"
#include "mathutil.h"
#include "quad-utils.h"
#include "tic.h"
#include "index_shard_internal.h"
#include "solver_field_geometry_internal.h"
#include "solver_inline_internal.h"
#include "solver_hypothesis_internal.h"
static uint64_t solver_order_double_bits(double value) {
    uint64_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

uint64_t solver_hypothesis_order_digest(
    const int* stars,
    const double* code,
    int dimquad,
    anbool current_parity) {
    uint64_t digest = 0;
    int dimcode = (dimquad - NBACK) * 2;
    int i;

    digest = solver_order_hash_mix(
        digest,
        UINT64_C(0x4859504f54484553));
    digest = solver_order_hash_mix(
        digest,
        (uint64_t)(unsigned int)dimquad);
    digest = solver_order_hash_mix(
        digest,
        (uint64_t)(unsigned int)current_parity);
    for (i = 0; i < dimquad; i++) {
        digest = solver_order_hash_mix(
            digest,
            (uint64_t)(unsigned int)stars[i]);
    }
    for (i = 0; i < dimcode; i++) {
        digest = solver_order_hash_mix(
            digest,
            solver_order_double_bits(code[i]));
    }
    return digest;
}

uint64_t solver_kd_result_order_digest(
    const kdtree_qres_t* result) {
    uint64_t digest = 0;
    int i;

    digest = solver_order_hash_mix(
        digest,
        UINT64_C(0x4b44524553554c54));
    digest = solver_order_hash_mix(
        digest,
        (uint64_t)(unsigned int)result->nres);
    for (i = 0; i < result->nres; i++) {
        digest = solver_order_hash_mix(
            digest,
            (uint64_t)(unsigned int)result->inds[i]);
        digest = solver_order_hash_mix(
            digest,
            solver_order_double_bits(result->sdists[i]));
    }
    return digest;
}

void solver_record_candidate_order(
    solver_t* solver,
    solver_ab_candidate_action_t action,
    int quadno,
    double code_err) {
    uint64_t digest = 0;

    if (!solver->profile.detailed) {
        return;
    }
    digest = solver_order_hash_mix(
        digest,
        UINT64_C(0x43414e4449444154));
    digest = solver_order_hash_mix(
        digest,
        (uint64_t)(unsigned int)action);
    digest = solver_order_hash_mix(
        digest,
        (uint64_t)(unsigned int)quadno);
    digest = solver_order_hash_mix(
        digest,
        solver_order_double_bits(code_err));
    solver->profile.candidate_order_hash =
        solver_order_hash_mix(
            solver->profile.candidate_order_hash,
            digest);
}

anbool solver_ab_poll_phase_stop(
    solver_t* solver,
    time_t* next_timer_callback_time) {
    time_t delay;
    time_t now;

    if (solver->quit_now ||
        solver_poll_worker_stop(solver)) {
        return TRUE;
    }
    if (!solver->timer_callback ||
        !next_timer_callback_time) {
        return FALSE;
    }
    now = time(NULL);
    if (now <= *next_timer_callback_time) {
        return FALSE;
    }
    update_timeused(solver);
    delay = solver->timer_callback(solver->userdata);
    if (!delay || solver->quit_now) {
        solver->quit_now = TRUE;
        return TRUE;
    }
    *next_timer_callback_time = now + delay;
    return FALSE;
}

/*
 * Descriptor planning is index-free. A synchronous packet may borrow a const
 * CodeKD tree view; QuadFile, StarKD, lazy index initialization, verification,
 * counters, callbacks, and reducer state remain owner-local.
 */
unsigned long long solver_ab_saturating_add(
    unsigned long long a,
    unsigned long long b) {
    if (ULLONG_MAX - a < b) {
        return ULLONG_MAX;
    }
    return a + b;
}

static unsigned long long solver_ab_saturating_choose(int n, int k) {
    unsigned long long result = 1;
    int i;

    if (n < 0 || k < 0 || k > n) {
        return 0;
    }
    if (k > n - k) {
        k = n - k;
    }
    for (i = 1; i <= k; i++) {
        unsigned long long numerator =
            (unsigned long long)(n - k + i);

        if (result > ULLONG_MAX / numerator) {
            return ULLONG_MAX;
        }
        result *= numerator;
        result /= (unsigned long long)i;
    }
    return result;
}

static anbool solver_ab_cancelled(
    const solver_ab_descriptor_planner_t* executor) {
    (void)executor;
    return index_shard_worker_stop_requested();
}

static int solver_ab_record_descriptor(
    solver_ab_builder_t* builder,
    const int* stars,
    const double* code,
    int dimquad,
    anbool current_parity) {
    solver_ab_descriptor_output_t* output =
        builder->descriptor_output;
    solver_ab_descriptor_t* descriptor;
    int dimcode = (dimquad - NBACK) * 2;

    if (!output || !stars || !code ||
        dimquad < NBACK || dimquad > DQMAX ||
        dimcode < 0 || dimcode > DCMAX ||
        output->descriptor_count >=
            SOLVER_AB_DESCRIPTOR_CAPACITY) {
        builder->fatal_error = TRUE;
        if (builder->status) {
            builder->status->evaluation_failed = TRUE;
        }
        return -1;
    }
    descriptor = &output->descriptors[
        output->descriptor_count++];
    memcpy(
        descriptor->stars,
        stars,
        (size_t)dimquad * sizeof(*stars));
    memcpy(
        descriptor->code,
        code,
        (size_t)dimcode * sizeof(*code));
    descriptor->tol2 = builder->tol2;
    descriptor->rel_field_noise2 =
        builder->rel_field_noise2;
    descriptor->numtries_delta =
        builder->pending_numtries;
    descriptor->cxdx_delta = builder->pending_cxdx;
    descriptor->meanx_delta = builder->pending_meanx;
    descriptor->current_parity = current_parity;
    builder->pending_numtries = 0U;
    builder->pending_cxdx = 0U;
    builder->pending_meanx = 0U;
    return 0;
}

static int solver_ab_record_hypothesis(
    solver_ab_builder_t* builder,
    const int* stars,
    const double* code,
    int dimquad,
    anbool current_parity) {
    if (!builder || !builder->status ||
        index_shard_worker_stop_requested()) {
        if (builder && builder->status) {
            builder->status->cancelled = TRUE;
        }
        return -1;
    }
    return solver_ab_record_descriptor(
        builder,
        stars,
        code,
        dimquad,
        current_parity);
}

static void solver_ab_try_permutations(
    const int* origstars,
    int dimquad,
    const double* origcode,
    solver_ab_builder_t* builder,
    anbool current_parity,
    int* stars,
    double* code,
    int slot,
    anbool* placed) {
    const solver_ab_snapshot_t* snapshot =
        &builder->planner->snapshot;
    double mycode[DCMAX];
    int nstars = dimquad - NBACK;
    int lastslot = dimquad - NBACK - 1;
    int i;

    if (!code) {
        code = mycode;
    }
    if (slot >= DCMAX / 2 ||
        solver_ab_cancelled(builder->planner) ||
        builder->fatal_error) {
        if (solver_ab_cancelled(builder->planner)) {
            builder->status->cancelled = TRUE;
        }
        return;
    }

    for (i = 0; i < nstars; i++) {
        if (placed[i]) {
            continue;
        }
        if (slot > 0 &&
            snapshot->cx_less_than_dx &&
            code[2 * (slot - 1)] >
                origcode[2 * i] + snapshot->cxdx_margin) {
            builder->pending_cxdx++;
            continue;
        }

        stars[slot + NBACK] = origstars[i + NBACK];
        code[2 * slot] = origcode[2 * i];
        code[2 * slot + 1] = origcode[2 * i + 1];

        if (snapshot->cx_less_than_dx &&
            snapshot->meanx_less_than_half) {
            double meanx = 0.0;
            int j;

            for (j = 0; j <= slot; j++) {
                meanx += code[2 * j];
            }
            meanx /= (double)(slot + 1);
            if (meanx > 0.5 + snapshot->cxdx_margin) {
                builder->pending_meanx++;
                continue;
            }
        }

        if (slot < lastslot) {
            placed[i] = TRUE;
            solver_ab_try_permutations(
                origstars,
                dimquad,
                origcode,
                builder,
                current_parity,
                stars,
                code,
                slot + 1,
                placed);
            placed[i] = FALSE;
            if (builder->fatal_error ||
                builder->status->cancelled) {
                return;
            }
        } else if (solver_ab_record_hypothesis(
                       builder,
                       stars,
                       code,
                       dimquad,
                       current_parity)) {
            return;
        }
    }
}

static void solver_ab_try_all_codes_2(
    const int* fieldstars,
    int dimquad,
    const double* code,
    solver_ab_builder_t* builder,
    anbool current_parity) {
    double flipcode[DCMAX];
    int stars[DQMAX];
    anbool placed[DQMAX];
    int dimcode = (dimquad - NBACK) * 2;
    int i;

    stars[0] = fieldstars[0];
    stars[1] = fieldstars[1];
    memset(placed, 0, sizeof(placed));
    solver_ab_try_permutations(
        fieldstars,
        dimquad,
        code,
        builder,
        current_parity,
        stars,
        NULL,
        0,
        placed);
    if (builder->fatal_error ||
        builder->status->cancelled) {
        return;
    }

    stars[0] = fieldstars[1];
    stars[1] = fieldstars[0];
    for (i = 0; i < dimcode; i++) {
        flipcode[i] = 1.0 - code[i];
    }
    memset(placed, 0, sizeof(placed));
    solver_ab_try_permutations(
        fieldstars,
        dimquad,
        flipcode,
        builder,
        current_parity,
        stars,
        NULL,
        0,
        placed);
}

static void solver_ab_try_all_codes(
    const solver_pair_geometry_t* pair_geometry,
    int field_a,
    int field_b,
    const int* fieldstars,
    int dimquad,
    solver_ab_builder_t* builder) {
    const solver_field_geometry_t* geometry =
        builder->planner->field_geometry;
    const solver_ab_snapshot_t* snapshot =
        &builder->planner->snapshot;
    double code[DCMAX];
    double flipcode[DCMAX];
    int dimcode = (dimquad - 2) * 2;
    int i;

    builder->pending_numtries++;
    for (i = 0; i < dimquad - NBACK; i++) {
        if (!solver_pair_geometry_transform(
                geometry,
                pair_geometry,
                field_a,
                field_b,
                fieldstars[NBACK + i],
                &code[2 * i],
                &code[2 * i + 1])) {
            builder->fatal_error = TRUE;
            builder->status->evaluation_failed = TRUE;
            return;
        }
    }

    if (snapshot->parity == PARITY_NORMAL ||
        snapshot->parity == PARITY_BOTH) {
        solver_ab_try_all_codes_2(
            fieldstars,
            dimquad,
            code,
            builder,
            FALSE);
    }
    if (builder->fatal_error ||
        builder->status->cancelled) {
        return;
    }
    if (snapshot->parity == PARITY_FLIP ||
        snapshot->parity == PARITY_BOTH) {
        quad_flip_parity(code, flipcode, dimcode);
        solver_ab_try_all_codes_2(
            fieldstars,
            dimquad,
            flipcode,
            builder,
            TRUE);
    }
}

typedef anbool (*solver_ab_combination_visitor_t)(
    const solver_pair_geometry_t* pair_geometry,
    const solver_ab_pair_t* pair,
    int* field,
    int dimquad,
    void* opaque);

static int solver_ab_select_combination(
    const int* eligible,
    int eligible_count,
    int* field,
    int fieldoffset,
    int n_to_add,
    int* selection,
    unsigned long long ordinal) {
    int position;

    for (position = 0; position < n_to_add; position++) {
        int remaining = n_to_add - position - 1;
        int bottom = position ?
            selection[position - 1] + 1 : 0;
        int candidate_index;
        anbool selected = FALSE;

        for (candidate_index = bottom;
             candidate_index < eligible_count;
             candidate_index++) {
            unsigned long long suffix_count;

            suffix_count = solver_ab_saturating_choose(
                eligible_count - candidate_index - 1,
                remaining);
            if (ordinal >= suffix_count) {
                ordinal -= suffix_count;
                continue;
            }
            selection[position] = candidate_index;
            field[fieldoffset + position] =
                eligible[candidate_index];
            selected = TRUE;
            break;
        }
        if (!selected) {
            return -1;
        }
    }
    return ordinal == 0U ? 0 : -1;
}

static anbool solver_ab_next_combination(
    const int* eligible,
    int eligible_count,
    int* field,
    int fieldoffset,
    int n_to_add,
    int* selection) {
    int position;

    for (position = n_to_add - 1;
         position >= 0;
         position--) {
        int maximum =
            eligible_count - (n_to_add - position);
        int fill;

        if (selection[position] >= maximum) {
            continue;
        }
        selection[position]++;
        field[fieldoffset + position] =
            eligible[selection[position]];
        for (fill = position + 1;
             fill < n_to_add;
             fill++) {
            selection[fill] = selection[fill - 1] + 1;
            field[fieldoffset + fill] =
                eligible[selection[fill]];
        }
        return TRUE;
    }
    return FALSE;
}

static int* solver_ab_get_eligible_workspace(
    solver_ab_descriptor_planner_t* planner,
    size_t required) {
    if (!planner || !planner->combination_eligible ||
        required > planner->combination_eligible_capacity) {
        return NULL;
    }
    return planner->combination_eligible;
}

static int solver_ab_visit_pair_range(
    solver_ab_descriptor_planner_t* executor,
    const solver_ab_pair_t* pair,
    const solver_pair_geometry_t* pair_geometry,
    unsigned long long local_first,
    unsigned long long local_end,
    solver_ab_combination_visitor_t visitor,
    void* opaque) {
    int field[DQMAX] = {0};
    int selection[DQMAX] = {0};
    int* eligible;
    int eligible_count = 0;
    int fieldoffset;
    int n_to_add;
    int i;
    unsigned long long ordinal;

    if (local_first >= local_end ||
        local_end > pair->combination_count) {
        return -1;
    }
    field[A] = pair->field_a;
    field[B] = pair->field_b;
    if (executor->phase == SOLVER_AB_PHASE_DIAGONAL) {
        fieldoffset = C;
        n_to_add = executor->dimquads - 2;
    } else {
        field[C] = executor->newpoint;
        fieldoffset = D;
        n_to_add = executor->dimquads - 3;
    }
    if (n_to_add < 0 ||
        fieldoffset + n_to_add > DQMAX) {
        return -1;
    }
    if (n_to_add == 0) {
        if (local_first != 0U ||
            local_end != 1U) {
            return -1;
        }
        return visitor(
            pair_geometry,
            pair,
            field,
            executor->dimquads,
            opaque) ? 1 : 0;
    }
    eligible = solver_ab_get_eligible_workspace(
        executor,
        (size_t)executor->newpoint);
    if (!eligible) {
        return -1;
    }
    for (i = 0; i < executor->newpoint; i++) {
        double x;
        double y;

        if (solver_pair_geometry_transform(
                executor->field_geometry,
                pair_geometry,
                pair->field_a,
                pair->field_b,
                i,
                &x,
                &y)) {
            eligible[eligible_count++] = i;
        }
    }
    if (solver_ab_saturating_choose(
            eligible_count,
            n_to_add) != pair->combination_count) {
        return -1;
    }
    if (solver_ab_select_combination(
            eligible,
            eligible_count,
            field,
            fieldoffset,
            n_to_add,
            selection,
            local_first)) {
        return -1;
    }
    for (ordinal = local_first;
         ordinal < local_end;
         ordinal++) {
        if (visitor(
                pair_geometry,
                pair,
                field,
                executor->dimquads,
                opaque)) {
            return 1;
        }
        if (ordinal + 1U < local_end &&
            !solver_ab_next_combination(
                eligible,
                eligible_count,
                field,
                fieldoffset,
                n_to_add,
                selection)) {
            return -1;
        }
    }
    return 0;
}

static size_t solver_ab_find_pair_for_work(
    const solver_ab_pair_t* pairs,
    size_t pair_count,
    unsigned long long work) {
    size_t low = 0U;
    size_t high = pair_count;

    while (low < high) {
        size_t middle = low + (high - low) / 2U;

        if (pairs[middle].combination_first <= work) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    return low ? low - 1U : 0U;
}

static anbool solver_ab_builder_visit(
    const solver_pair_geometry_t* pair_geometry,
    const solver_ab_pair_t* pair,
    int* field,
    int dimquad,
    void* opaque) {
    solver_ab_builder_t* builder = opaque;

    if (solver_ab_cancelled(builder->planner)) {
        builder->status->cancelled = TRUE;
        return TRUE;
    }
    solver_ab_try_all_codes(
        pair_geometry,
        pair->field_a,
        pair->field_b,
        field,
        dimquad,
        builder);
    return builder->fatal_error ||
        builder->status->cancelled;
}

typedef int (*solver_ab_pair_range_visitor_t)(
    solver_ab_descriptor_planner_t* executor,
    const solver_ab_pair_t* pair,
    const solver_pair_geometry_t* pair_geometry,
    unsigned long long local_first,
    unsigned long long local_end,
    void* opaque);

static int solver_ab_walk_task_ranges(
    solver_ab_descriptor_planner_t* executor,
    const solver_ab_task_t* task,
    solver_ab_pair_range_visitor_t visitor,
    void* opaque) {
    unsigned long long cursor;
    size_t pair_index;

    if (!executor || !task || !visitor ||
        !executor->pairs || !executor->pair_count ||
        task->combination_first >=
            task->combination_end) {
        return -1;
    }
    cursor = task->combination_first;
    pair_index = solver_ab_find_pair_for_work(
        executor->pairs,
        executor->pair_count,
        cursor);
    for (;
         pair_index < executor->pair_count &&
             cursor < task->combination_end;
         pair_index++) {
        const solver_ab_pair_t* pair =
            &executor->pairs[pair_index];
        unsigned long long pair_end;
        unsigned long long slice_end;
        const solver_pair_geometry_t* pair_geometry;
        int visit_result;

        if (!pair->combination_count ||
            ULLONG_MAX - pair->combination_first <
                pair->combination_count) {
            return -1;
        }
        pair_end = pair->combination_first +
            pair->combination_count;
        if (pair_end <= cursor) {
            continue;
        }
        if (pair->combination_first > cursor) {
            return -1;
        }
        slice_end = MIN(
            task->combination_end,
            pair_end);
        pair_geometry = solver_field_geometry_pair(
            executor->field_geometry,
            pair->field_a,
            pair->field_b);
        if (!pair_geometry) {
            return -1;
        }
        visit_result = visitor(
            executor,
            pair,
            pair_geometry,
            cursor - pair->combination_first,
            slice_end - pair->combination_first,
            opaque);
        if (visit_result) {
            return visit_result;
        }
        cursor = slice_end;
    }
    return cursor == task->combination_end ? 0 : -1;
}

static int solver_ab_builder_visit_pair_range(
    solver_ab_descriptor_planner_t* executor,
    const solver_ab_pair_t* pair,
    const solver_pair_geometry_t* pair_geometry,
    unsigned long long local_first,
    unsigned long long local_end,
    void* opaque) {
    solver_ab_builder_t* builder = opaque;

    if (solver_ab_cancelled(executor)) {
        builder->status->cancelled = TRUE;
        return 1;
    }
    builder->tol2 = pair->tol2;
    builder->rel_field_noise2 =
        pair_geometry->rel_field_noise2;
    builder->rel_field_noise_valid = TRUE;
    return solver_ab_visit_pair_range(
        executor,
        pair,
        pair_geometry,
        local_first,
        local_end,
        solver_ab_builder_visit,
        builder);
}

static int solver_ab_counter_failure(
    solver_t* solver,
    const char* counter_name) {
    logerr(
        "[solver-ab] signed counter boundary reached: %s\n",
        counter_name ? counter_name : "(unknown)");
    if (solver) {
        solver->profile.execution_failed = TRUE;
        solver->quit_now = TRUE;
    }
    return -1;
}

static anbool solver_ab_counter_can_add(
    int current,
    unsigned long long delta) {
    if (current < 0 ||
        delta > (unsigned long long)INT_MAX) {
        return FALSE;
    }
    return delta <=
        (unsigned long long)(INT_MAX - current);
}

int solver_ab_checked_counter_delta(
    solver_t* solver,
    unsigned long long numtries,
    unsigned long long cxdx,
    unsigned long long meanx) {
    if (!solver) {
        return -1;
    }
    /*
     * Preflight every destination before mutating any of them. This preserves
     * an exact reducer prefix even at the representable counter boundary.
     */
    if (!solver_ab_counter_can_add(
            solver->numtries,
            numtries)) {
        return solver_ab_counter_failure(
            solver,
            "numtries");
    }
    if (!solver_ab_counter_can_add(
            solver->num_cxdx_skipped,
            cxdx)) {
        return solver_ab_counter_failure(
            solver,
            "num_cxdx_skipped");
    }
    if (!solver_ab_counter_can_add(
            solver->num_meanx_skipped,
            meanx)) {
        return solver_ab_counter_failure(
            solver,
            "num_meanx_skipped");
    }
    solver->numtries += (int)numtries;
    solver->num_cxdx_skipped += (int)cxdx;
    solver->num_meanx_skipped += (int)meanx;
    return 0;
}

static int solver_ab_reserve_pair_buffer(
    solver_ab_descriptor_planner_t* executor,
    size_t required) {
    solver_ab_pair_t* resized;
    size_t capacity;
    size_t retained_maximum;

    if (!executor ||
        required > SIZE_MAX / sizeof(*resized)) {
        return -1;
    }
    if (!executor->pair_cache_limit_bytes) {
        executor->pair_cache_limit_bytes =
            SOLVER_AB_DESCRIPTOR_PAIR_CACHE_BYTES;
    }
    if (required <= executor->pair_capacity) {
        return 0;
    }
    capacity = executor->pair_capacity ?
        executor->pair_capacity : 64U;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            capacity = required;
        } else {
            capacity *= 2U;
        }
    }

    retained_maximum =
        executor->pair_cache_limit_bytes /
        sizeof(*resized);
    if (!executor->pairs_transient &&
        required <= retained_maximum) {
        if (capacity > retained_maximum) {
            capacity = retained_maximum;
        }
        resized = realloc(
            executor->pair_cache,
            capacity * sizeof(*resized));
        if (!resized) {
            return -1;
        }
        executor->pair_cache = resized;
        executor->pair_cache_capacity = capacity;
        executor->pairs = resized;
        executor->pair_capacity = capacity;
        return 0;
    }

    if (capacity > SIZE_MAX / sizeof(*resized)) {
        capacity = required;
    }
    if (executor->pairs_transient) {
        resized = realloc(
            executor->pairs,
            capacity * sizeof(*resized));
    } else {
        resized = malloc(capacity * sizeof(*resized));
        if (resized && required > 1U && executor->pairs) {
            memcpy(
                resized,
                executor->pairs,
                (required - 1U) * sizeof(*resized));
        }
    }
    if (!resized) {
        return -1;
    }
    executor->pairs = resized;
    executor->pair_capacity = capacity;
    executor->pairs_transient = TRUE;
    return 0;
}

static void solver_ab_begin_pair_buffer(
    solver_ab_descriptor_planner_t* executor) {
    executor->pairs = executor->pair_cache;
    executor->pair_capacity =
        executor->pair_cache_capacity;
    executor->pairs_transient = FALSE;
}

int solver_ab_collect_pairs(
    solver_ab_descriptor_planner_t* executor,
    solver_ab_phase_kind_t phase,
    int newpoint,
    int dimquads,
    const solver_field_geometry_t* geometry,
    double min_ab2,
    double max_ab2,
    solver_ab_pair_t** pairs_out,
    size_t* pair_count_out,
    unsigned long long* total_combinations_out) {
    size_t count = 0U;
    unsigned long long total_combinations = 0;
    int field_a;
    int field_b;

    if (!executor || executor->pairs ||
        executor->pair_count != 0U) {
        return -1;
    }
    solver_ab_begin_pair_buffer(executor);
    if (newpoint <= 0) {
        *pairs_out = NULL;
        *pair_count_out = 0U;
        *total_combinations_out = 0U;
        return 0;
    }

    if (phase == SOLVER_AB_PHASE_DIAGONAL) {
        field_b = newpoint;
        for (field_a = 0;
             field_a < newpoint;
             field_a++) {
            const solver_pair_geometry_t* pair_geometry =
                solver_field_geometry_pair(
                    geometry,
                    field_a,
                    field_b);
            unsigned long long combination_count;

            if (!pair_geometry ||
                !pair_geometry->scale_ok ||
                pair_geometry->scale < min_ab2 ||
                pair_geometry->scale > max_ab2) {
                continue;
            }
            combination_count =
                solver_ab_saturating_choose(
                solver_pair_geometry_eligible_before(
                    geometry,
                    pair_geometry,
                    field_a,
                    field_b,
                    newpoint),
                dimquads - 2);
            if (!combination_count) {
                continue;
            }
            if (solver_ab_reserve_pair_buffer(
                    executor, count + 1U)) {
                return -1;
            }
            executor->pairs[count].field_a = field_a;
            executor->pairs[count].field_b = field_b;
            executor->pairs[count].combination_first =
                total_combinations;
            executor->pairs[count].combination_count =
                combination_count;
            executor->pairs[count].tol2 = get_tolerance_for_noise(
                executor->snapshot.codetol,
                pair_geometry->rel_field_noise2,
                executor->snapshot.rel_index_noise2);
            count++;
            total_combinations =
                solver_ab_saturating_add(
                    total_combinations,
                    combination_count);
        }
    } else {
        for (field_a = 0;
             field_a < newpoint;
             field_a++) {
            for (field_b = field_a + 1;
                 field_b < newpoint;
                 field_b++) {
                const solver_pair_geometry_t* pair_geometry =
                    solver_field_geometry_pair(
                        geometry,
                        field_a,
                        field_b);
                unsigned long long combination_count;
                double newpoint_x;
                double newpoint_y;

                if (!pair_geometry ||
                    !pair_geometry->scale_ok ||
                    pair_geometry->scale < min_ab2 ||
                    pair_geometry->scale > max_ab2 ||
                    !solver_pair_geometry_transform(
                        geometry,
                        pair_geometry,
                        field_a,
                        field_b,
                        newpoint,
                        &newpoint_x,
                        &newpoint_y)) {
                    continue;
                }
                combination_count = dimquads > 3 ?
                    solver_ab_saturating_choose(
                        solver_pair_geometry_eligible_before(
                            geometry,
                            pair_geometry,
                            field_a,
                            field_b,
                            newpoint),
                        dimquads - 3) :
                    1U;
                if (!combination_count) {
                    continue;
                }
                if (solver_ab_reserve_pair_buffer(
                        executor, count + 1U)) {
                    return -1;
                }
                executor->pairs[count].field_a = field_a;
                executor->pairs[count].field_b = field_b;
                executor->pairs[count].combination_first =
                    total_combinations;
                executor->pairs[count].combination_count =
                    combination_count;
                executor->pairs[count].tol2 = get_tolerance_for_noise(
                    executor->snapshot.codetol,
                    pair_geometry->rel_field_noise2,
                    executor->snapshot.rel_index_noise2);
                count++;
                total_combinations =
                    solver_ab_saturating_add(
                        total_combinations,
                        combination_count);
            }
        }
    }

    if (!count) {
        *pairs_out = NULL;
        *pair_count_out = 0U;
        *total_combinations_out = 0U;
        return 0;
    }
    *pairs_out = executor->pairs;
    *pair_count_out = count;
    *total_combinations_out = total_combinations;
    return 0;
}

#define SOLVER_VERIFICATION_WAVE_MAX_BYTES \
    (32U * 1024U * 1024U)
size_t solver_verification_wave_memory_budget(void) {
    size_t budget = SOLVER_VERIFICATION_WAVE_MAX_BYTES;

#if defined(_SC_AVPHYS_PAGES)
    long available_pages = sysconf(_SC_AVPHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);

    if (available_pages > 0 && page_size > 0 &&
        (unsigned long)available_pages <=
            SIZE_MAX / (unsigned long)page_size) {
        size_t pressure_budget =
            ((size_t)available_pages * (size_t)page_size) / 64U;

        budget = MIN(budget, pressure_budget);
    }
#endif
    return budget;
}

static pthread_key_t solver_ab_descriptor_workspace_key;
static pthread_once_t solver_ab_descriptor_workspace_once =
    PTHREAD_ONCE_INIT;
static int solver_ab_descriptor_workspace_status = EAGAIN;

void solver_ab_descriptor_release_pairs(
    solver_ab_descriptor_workspace_t* workspace) {
    solver_ab_descriptor_planner_t* planner;

    if (!workspace) {
        return;
    }
    planner = &workspace->planner;
    if (planner->pairs_transient) {
        free(planner->pairs);
    }
    planner->pairs = NULL;
    planner->pair_count = 0U;
    planner->pair_capacity = 0U;
    planner->pairs_transient = FALSE;
}

static void solver_ab_descriptor_workspace_destroy(void* opaque) {
    solver_ab_descriptor_workspace_t* workspace = opaque;

    if (!workspace) {
        return;
    }
    solver_ab_descriptor_release_pairs(workspace);
    free(workspace->planner.pair_cache);
    free(workspace->outputs);
    free(workspace);
}

static void solver_ab_descriptor_workspace_make_key(void) {
    solver_ab_descriptor_workspace_status =
        pthread_key_create(
            &solver_ab_descriptor_workspace_key,
            solver_ab_descriptor_workspace_destroy);
}

int solver_ab_descriptor_workspace_reserve_outputs(
    solver_ab_descriptor_workspace_t* workspace,
    size_t output_count) {
    solver_ab_descriptor_output_t* outputs;

    if (!workspace || !output_count ||
        output_count > SIZE_MAX / sizeof(*outputs)) {
        return -1;
    }
    if (output_count <= workspace->output_capacity) {
        return 0;
    }
    outputs = calloc(output_count, sizeof(*outputs));
    if (!outputs) {
        return -1;
    }
    free(workspace->outputs);
    workspace->outputs = outputs;
    workspace->output_capacity = output_count;
    return 0;
}

solver_ab_descriptor_workspace_t*
solver_ab_descriptor_workspace_get(void) {
    solver_ab_descriptor_workspace_t* workspace;

    if (pthread_once(
            &solver_ab_descriptor_workspace_once,
            solver_ab_descriptor_workspace_make_key) ||
        solver_ab_descriptor_workspace_status) {
        return NULL;
    }
    workspace = pthread_getspecific(
        solver_ab_descriptor_workspace_key);
    if (workspace) {
        return workspace;
    }
    workspace = calloc(1, sizeof(*workspace));
    if (!workspace) {
        return NULL;
    }
    workspace->planner.pair_cache_limit_bytes =
        SOLVER_AB_DESCRIPTOR_PAIR_CACHE_BYTES;
    if (pthread_setspecific(
            solver_ab_descriptor_workspace_key,
            workspace)) {
        solver_ab_descriptor_workspace_destroy(workspace);
        return NULL;
    }
    return workspace;
}

index_shard_helper_task_status_t
solver_ab_descriptor_helper_execute(
    const void* input_bytes,
    size_t input_size,
    void* output_bytes,
    size_t output_size) {
    const solver_ab_descriptor_task_input_t* input = input_bytes;
    const solver_ab_descriptor_phase_context_t* phase;
    solver_ab_descriptor_output_t* output = output_bytes;
    solver_ab_descriptor_planner_t executor;
    solver_ab_builder_t builder;
    solver_descriptor_status_t status;
    solver_ab_task_t task;
    int eligible[SOLVER_AB_DESCRIPTOR_MAX_FIELD_OBJECTS];
    int visit_result;

    phase = input ? input->phase : NULL;
    if (!phase ||
        input_size != sizeof(*input) ||
        !output ||
        output_size != sizeof(*output) ||
        !phase->field_geometry ||
        !phase->pairs || !phase->pair_count ||
        input->combination_first >= input->combination_end ||
        phase->newpoint < 0 ||
        phase->newpoint >=
            SOLVER_AB_DESCRIPTOR_MAX_FIELD_OBJECTS ||
        phase->dimquads < NBACK ||
        phase->dimquads > DQMAX ||
        (phase->phase != SOLVER_AB_PHASE_DIAGONAL &&
         phase->phase != SOLVER_AB_PHASE_OFF_DIAGONAL) ||
        (phase->parity != PARITY_NORMAL &&
         phase->parity != PARITY_FLIP &&
         phase->parity != PARITY_BOTH)) {
        return INDEX_SHARD_HELPER_TASK_ERROR;
    }

    memset(&executor, 0, sizeof(executor));
    memset(&builder, 0, sizeof(builder));
    memset(&status, 0, sizeof(status));
    memset(&task, 0, sizeof(task));
    output->descriptor_count = 0U;
    output->trailing_numtries = 0U;
    output->trailing_cxdx = 0U;
    output->trailing_meanx = 0U;
    output->has_final_rel_field_noise2 = FALSE;

    executor.field_geometry = phase->field_geometry;
    executor.newpoint = phase->newpoint;
    executor.dimquads = phase->dimquads;
    executor.phase = phase->phase;
    executor.pairs = (solver_ab_pair_t*)phase->pairs;
    executor.pair_count = phase->pair_count;
    executor.combination_eligible = eligible;
    executor.combination_eligible_capacity =
        SOLVER_AB_DESCRIPTOR_MAX_FIELD_OBJECTS;
    executor.snapshot.parity = phase->parity;
    executor.snapshot.cx_less_than_dx =
        phase->cx_less_than_dx;
    executor.snapshot.meanx_less_than_half =
        phase->meanx_less_than_half;
    executor.snapshot.cxdx_margin = phase->cxdx_margin;

    builder.planner = &executor;
    builder.status = &status;
    builder.descriptor_output = output;
    task.combination_first = input->combination_first;
    task.combination_end = input->combination_end;
    visit_result = solver_ab_walk_task_ranges(
        &executor,
        &task,
        solver_ab_builder_visit_pair_range,
        &builder);
    if (status.cancelled ||
        index_shard_worker_stop_requested()) {
        return INDEX_SHARD_HELPER_TASK_STOPPED;
    }
    if (visit_result < 0 ||
        builder.fatal_error ||
        status.evaluation_failed) {
        return INDEX_SHARD_HELPER_TASK_ERROR;
    }
    output->trailing_numtries = builder.pending_numtries;
    output->trailing_cxdx = builder.pending_cxdx;
    output->trailing_meanx = builder.pending_meanx;
    output->final_rel_field_noise2 =
        builder.rel_field_noise2;
    output->has_final_rel_field_noise2 =
        builder.rel_field_noise_valid;
    return INDEX_SHARD_HELPER_TASK_OK;
}

size_t solver_ab_descriptor_expansion(
    int dimquads,
    int parity) {
    size_t expansion = 2U;
    int internal_stars = dimquads - NBACK;
    int i;

    if (dimquads < NBACK || dimquads > DQMAX ||
        (parity != PARITY_NORMAL &&
         parity != PARITY_FLIP &&
         parity != PARITY_BOTH)) {
        return 0U;
    }
    if (parity == PARITY_BOTH) {
        expansion *= 2U;
    }
    for (i = 2; i <= internal_stars; i++) {
        expansion *= (size_t)i;
    }
    return expansion;
}

size_t solver_ab_descriptor_partition_count(
    unsigned long long wave_combinations,
    size_t expansion,
    size_t max_task_combinations,
    size_t participants) {
    unsigned long long expanded_work;
    unsigned long long capacity_tasks;
    unsigned long long work_tasks;
    unsigned long long task_count;

    if (!wave_combinations || !expansion ||
        !max_task_combinations || !participants ||
        wave_combinations > ULLONG_MAX /
            (unsigned long long)expansion) {
        return 0U;
    }
    expanded_work = wave_combinations *
        (unsigned long long)expansion;
    capacity_tasks = wave_combinations /
        (unsigned long long)max_task_combinations;
    if (wave_combinations %
            (unsigned long long)max_task_combinations) {
        capacity_tasks++;
    }
    if (!capacity_tasks ||
        capacity_tasks > (unsigned long long)participants) {
        return 0U;
    }
    work_tasks = expanded_work /
        SOLVER_AB_DESCRIPTOR_TASK_TARGET_HYPOTHESES;
    if (expanded_work %
            SOLVER_AB_DESCRIPTOR_TASK_TARGET_HYPOTHESES) {
        work_tasks++;
    }
    work_tasks = MIN(
        work_tasks,
        (unsigned long long)participants);
    work_tasks = MIN(work_tasks, wave_combinations);
    task_count = MAX(capacity_tasks, work_tasks);
    if (!task_count ||
        task_count > (unsigned long long)participants ||
        task_count > (unsigned long long)SIZE_MAX) {
        return 0U;
    }
    return (size_t)task_count;
}
