/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */

#include "solver.h"
#include "log.h"
#include "index_shard_internal.h"
#include "solver_inline_internal.h"
#include "solver_profile_internal.h"

void solver_profile_accumulate(solver_profile_t* total,
                               const solver_profile_t* profile) {
    if (!total || !profile) {
        return;
    }

    total->detailed = total->detailed || profile->detailed;
    total->execution_failed =
        total->execution_failed || profile->execution_failed;

    total->codekd_calls += profile->codekd_calls;
    total->codekd_hits += profile->codekd_hits;
    total->resolve_calls += profile->resolve_calls;
    total->verify_calls += profile->verify_calls;
    total->allocation_failures += profile->allocation_failures;
    total->ab_helper_tasks += profile->ab_helper_tasks;
    total->candidate_delivery_windows +=
        profile->candidate_delivery_windows;
    if (!total->hypothesis_order_hash) {
        total->hypothesis_order_hash =
            profile->hypothesis_order_hash;
    } else if (profile->hypothesis_order_hash) {
        total->hypothesis_order_hash =
            solver_order_hash_mix(
                total->hypothesis_order_hash,
                profile->hypothesis_order_hash);
    }
    if (!total->kd_result_order_hash) {
        total->kd_result_order_hash =
            profile->kd_result_order_hash;
    } else if (profile->kd_result_order_hash) {
        total->kd_result_order_hash =
            solver_order_hash_mix(
                total->kd_result_order_hash,
                profile->kd_result_order_hash);
    }
    if (!total->candidate_order_hash) {
        total->candidate_order_hash =
            profile->candidate_order_hash;
    } else if (profile->candidate_order_hash) {
        total->candidate_order_hash =
            solver_order_hash_mix(
                total->candidate_order_hash,
                profile->candidate_order_hash);
    }

    if (profile->max_parallel_ranges >
        total->max_parallel_ranges) {
        total->max_parallel_ranges = profile->max_parallel_ranges;
    }
}

void solver_profile_report(const solver_t* solver) {
    const solver_profile_t* profile;

    if (!solver || !index_shard_trace_enabled() ||
        index_shard_worker_context_active()) {
        return;
    }

    profile = &solver->profile;
    logmsg("[solver] phase-profile detailed=%i failed=%i "
           "codekd_calls=%llu codekd_hits=%llu "
           "resolve_calls=%llu verify_calls=%llu "
           "allocation_failures=%llu helper_tasks=%llu "
           "max_parallel=%zu "
           "hypothesis_order=%016llx "
           "kd_result_order=%016llx candidate_order=%016llx\n",
           profile->detailed ? 1 : 0,
           profile->execution_failed ? 1 : 0,
           profile->codekd_calls,
           profile->codekd_hits,
           profile->resolve_calls,
           profile->verify_calls,
           profile->allocation_failures,
           profile->ab_helper_tasks,
           profile->max_parallel_ranges,
           profile->hypothesis_order_hash,
           profile->kd_result_order_hash,
           profile->candidate_order_hash);
    logmsg("[solver] page-pipeline windows=%llu\n",
           profile->candidate_delivery_windows);
}
