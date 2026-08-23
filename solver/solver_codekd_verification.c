/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */
/* Prepared verification queries, matching, and score windows. */

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "solver.h"
#include "verify.h"
#include "fitsbin.h"
#include "log.h"
#include "mathutil.h"
#include "tic.h"
#include "index_shard_internal.h"
#include "solver_codekd_internal.h"
#include "solver_inline_internal.h"
#include "solver_hypothesis_internal.h"
/*
 * Execute each native StarKD query once after its exact DATA/PERM pages are
 * ready. Retain the logical result order, then derive only the mapped sweep
 * pages that those results will dereference. Physical page keys may be
 * deduplicated and sorted; logical query results must never be reordered.
 */
index_shard_helper_task_status_t
solver_codekd_packet_query_verification_ready(
    solver_codekd_search_packet_t* packet,
    anbool* more_work) {
    solver_codekd_page_workspace_t* workspace;
    fitsbin_t* source;
    size_t plan_end;
    size_t segment_end;
    size_t candidate_index;
    size_t unique_pages = 0U;
    size_t range_count = 0U;
    size_t aligned_bytes = 0U;
    size_t captured_queries = 0U;
    int seal_status;

    if (!packet || !packet->phase || !more_work ||
        !packet->phase->starkd ||
        !packet->phase->starkd->tree || !packet->phase->starkd->tree->io ||
        !packet->phase->starkd->tree->io_is_fitsbin ||
        !packet->candidate_records || !packet->page_workspace ||
        packet->state !=
            SOLVER_CODEKD_PACKET_VERIFY_QUERY_COMPUTE_READY ||
        !packet->verify_plan_complete ||
        packet->verify_plan_first !=
            packet->candidate_window_offset ||
        packet->verify_plan_end <= packet->verify_plan_first ||
        packet->verify_plan_end >
            packet->candidate_window_count ||
        packet->verify_topology_end !=
            packet->verify_plan_end) {
        return INDEX_SHARD_HELPER_TASK_ERROR;
    }
    *more_work = FALSE;
    if (index_shard_worker_stop_requested()) {
        packet->state = SOLVER_CODEKD_PACKET_STOPPED;
        return INDEX_SHARD_HELPER_TASK_STOPPED;
    }
    workspace = packet->page_workspace;
    source = (fitsbin_t*)packet->phase->starkd->tree->io;
    /*
     * The descriptor scratch is now reused for exact sweep pages. Any
     * topology plan that was deferred beyond this prefix must be rebuilt
     * when owner retirement advances to it.
     */
    packet->verify_pending_query_plan = FALSE;
    packet->verify_pending_query_index = 0U;
    solver_codekd_page_set_reset(&workspace->descriptor);
    solver_codekd_page_set_reset(&workspace->group);
    plan_end = MIN(
        packet->verify_plan_end,
        packet->verify_plan_first +
            (size_t)SOLVER_VERIFY_QUERY_LOOKAHEAD);
    segment_end = packet->verify_plan_first;
    packet->verify_sweep_range_count = 0U;
    packet->verify_sweep_aligned_bytes = 0U;
    packet->verify_sweep_plan_complete = FALSE;

    for (candidate_index = packet->verify_plan_first;
         candidate_index < plan_end;
         candidate_index++) {
        solver_candidate_delivery_record_t* record =
            &packet->candidate_records[candidate_index];
        solver_codekd_page_plan_t plan;
        verify_index_query_t* query = NULL;
        size_t query_bytes;
        size_t result_index;
        anbool query_plan_failed = FALSE;

        if (record->plan_action !=
            SOLVER_AB_CANDIDATE_VERIFY) {
            segment_end = candidate_index + 1U;
            continue;
        }
        if (record->verify_query_captured) {
            if (!record->verify_query) {
                return INDEX_SHARD_HELPER_TASK_ERROR;
            }
            captured_queries++;
            segment_end = candidate_index + 1U;
            continue;
        }
        if (index_shard_worker_stop_requested()) {
            packet->state = SOLVER_CODEKD_PACKET_STOPPED;
            return INDEX_SHARD_HELPER_TASK_STOPPED;
        }
        query = record->verify_query;
        if (!query) {
            if (verify_query_hit(
                    packet->phase->starkd,
                    record->verify_center,
                    record->verify_radius2,
                    &query) ||
                !query) {
                segment_end = candidate_index + 1U;
                continue;
            }
            query_bytes = verify_index_query_bytes(query);
            if (query_bytes == SIZE_MAX ||
                query_bytes > packet->verify_query_budget ||
                packet->verify_query_bytes >
                    packet->verify_query_budget -
                        query_bytes) {
                verify_destroy_index_query(query);
                segment_end = candidate_index + 1U;
                continue;
            }
            record->verify_query = query;
            packet->verify_query_bytes += query_bytes;
        }

        solver_codekd_page_set_reset(&workspace->descriptor);
        memset(&plan, 0, sizeof(plan));
        plan.source = source;
        plan.workspace = workspace;
        plan.enabled = TRUE;
        for (result_index = 0U;
             result_index < verify_index_query_count(query);
             result_index++) {
            kdtree_prefetch_hint_t hint;
            const void* data = NULL;
            size_t size = 0U;
            int emit_status;

            if (!(result_index & 255U) &&
                index_shard_worker_stop_requested()) {
                packet->state =
                    SOLVER_CODEKD_PACKET_STOPPED;
                return INDEX_SHARD_HELPER_TASK_STOPPED;
            }
            if (verify_index_query_sweep_range(
                    packet->phase->starkd,
                    query,
                    result_index,
                    &data,
                    &size)) {
                query_plan_failed = TRUE;
                break;
            }
            memset(&hint, 0, sizeof(hint));
            hint.mapping = source;
            hint.address = data;
            hint.length = size;
            emit_status =
                solver_codekd_page_plan_emit(&plan, &hint);
            if (emit_status !=
                KDTREE_PREFETCH_EMIT_CONTINUE) {
                query_plan_failed = TRUE;
                break;
            }
        }
        if (query_plan_failed) {
            if (segment_end > packet->verify_plan_first) {
                break;
            }
            packet->verify_plan_end = candidate_index + 1U;
            packet->state =
                SOLVER_CODEKD_PACKET_VERIFY_COMPUTE_READY;
            *more_work = TRUE;
            return INDEX_SHARD_HELPER_TASK_OK;
        }

        seal_status = solver_codekd_page_set_union_fits(workspace);
        if (seal_status <= 0) {
            if (segment_end > packet->verify_plan_first) {
                break;
            }
            packet->verify_plan_end = candidate_index + 1U;
            packet->state =
                SOLVER_CODEKD_PACKET_VERIFY_COMPUTE_READY;
            *more_work = TRUE;
            return INDEX_SHARD_HELPER_TASK_OK;
        }
        if (solver_codekd_page_set_merge_descriptor(workspace)) {
            goto sweep_fallback;
        }
        segment_end = candidate_index + 1U;
        if (workspace->group.count >= workspace->page_limit) {
            break;
        }
    }

    if (segment_end <= packet->verify_plan_first) {
        goto sweep_fallback;
    }
    if (!workspace->group.count) {
        packet->verify_plan_end = segment_end;
        packet->verify_sweep_range_count = 0U;
        packet->verify_sweep_aligned_bytes = 0U;
        packet->verify_sweep_plan_complete = FALSE;
        packet->state = captured_queries
            ? SOLVER_CODEKD_PACKET_VERIFY_PREPARE_COMPUTE_READY
            : SOLVER_CODEKD_PACKET_RESULTS_READY;
        *more_work = captured_queries != 0U;
        return INDEX_SHARD_HELPER_TASK_OK;
    }
    seal_status = solver_codekd_page_plan_seal_union(
        workspace,
        FALSE,
        &unique_pages,
        &range_count,
        &aligned_bytes);
    if (seal_status) {
        goto sweep_fallback;
    }
    packet->verify_plan_end = segment_end;
    packet->verify_sweep_range_count = range_count;
    packet->verify_sweep_aligned_bytes = aligned_bytes;
    packet->verify_sweep_plan_complete = TRUE;
    packet->state = range_count
        ? SOLVER_CODEKD_PACKET_VERIFY_SWEEP_SUBMIT_READY
        : SOLVER_CODEKD_PACKET_VERIFY_SWEEP_COMPUTE_READY;
    *more_work = TRUE;
    return INDEX_SHARD_HELPER_TASK_OK;

sweep_fallback:
    packet->verify_sweep_range_count = 0U;
    packet->verify_sweep_aligned_bytes = 0U;
    packet->verify_sweep_plan_complete = FALSE;
    packet->state = SOLVER_CODEKD_PACKET_VERIFY_COMPUTE_READY;
    *more_work = TRUE;
    return INDEX_SHARD_HELPER_TASK_OK;
}

index_shard_helper_task_status_t
solver_codekd_packet_capture_sweep_ready(
    solver_codekd_search_packet_t* packet,
    anbool* more_work) {
    size_t candidate_index;
    size_t captured_queries = 0U;

    if (!packet || !packet->phase || !more_work ||
        !packet->phase->starkd ||
        !packet->candidate_records ||
        packet->state !=
            SOLVER_CODEKD_PACKET_VERIFY_SWEEP_COMPUTE_READY ||
        !packet->verify_plan_complete ||
        !packet->verify_sweep_plan_complete ||
        packet->verify_plan_first !=
            packet->candidate_window_offset ||
        packet->verify_plan_end <= packet->verify_plan_first ||
        packet->verify_plan_end >
            packet->candidate_window_count ||
        packet->verify_topology_end <
            packet->verify_plan_end ||
        packet->verify_topology_end >
            packet->candidate_window_count) {
        return INDEX_SHARD_HELPER_TASK_ERROR;
    }
    *more_work = FALSE;
    for (candidate_index = packet->verify_plan_first;
         candidate_index < packet->verify_plan_end;
         candidate_index++) {
        solver_candidate_delivery_record_t* record =
            &packet->candidate_records[candidate_index];

        if (index_shard_worker_stop_requested()) {
            packet->state = SOLVER_CODEKD_PACKET_STOPPED;
            return INDEX_SHARD_HELPER_TASK_STOPPED;
        }
        if (record->verify_query_captured) {
            if (!record->verify_query) {
                return INDEX_SHARD_HELPER_TASK_ERROR;
            }
            captured_queries++;
            continue;
        }
        if (record->verify_query) {
            if (!verify_index_query_capture_sweep(
                    packet->phase->starkd,
                    record->verify_query)) {
                record->verify_query_captured = TRUE;
                captured_queries++;
            }
        }
    }
    if (index_shard_worker_stop_requested()) {
        packet->state = SOLVER_CODEKD_PACKET_STOPPED;
        return INDEX_SHARD_HELPER_TASK_STOPPED;
    }
    packet->state = captured_queries
        ? SOLVER_CODEKD_PACKET_VERIFY_PREPARE_COMPUTE_READY
        : SOLVER_CODEKD_PACKET_RESULTS_READY;
    *more_work =
        packet->state ==
            SOLVER_CODEKD_PACKET_VERIFY_PREPARE_COMPUTE_READY;
    return INDEX_SHARD_HELPER_TASK_OK;
}

static int solver_codekd_packet_build_verification_match(
    const solver_codekd_packet_task_input_t* input,
    const solver_codekd_search_packet_t* packet,
    const solver_candidate_delivery_record_t* record,
    size_t candidate_index,
    MatchObj* match,
    double* verify_pix2,
    double* logaccept) {
    const solver_codekd_verification_snapshot_t* snapshot;
    const solver_codekd_result_slot_t* slot;
    size_t global_candidate;
    size_t slot_end;
    double radius2;
    int star_index;

    if (!input || !input->phase ||
        !packet || !packet->phase ||
        packet->phase != input->phase ||
        !record || !match ||
        !verify_pix2 || !logaccept ||
        candidate_index >= packet->candidate_window_count ||
        packet->candidate_window_first >
            packet->candidate_count ||
        candidate_index >=
            packet->candidate_count -
                packet->candidate_window_first ||
        record->descriptor_index >= packet->count ||
        record->plan_action != SOLVER_AB_CANDIDATE_VERIFY ||
        !record->candidate_prepared ||
        packet->phase->descriptor.dimquads <= 0 || packet->phase->descriptor.dimquads > DQMAX) {
        return -1;
    }
    snapshot = &input->phase->verification;
    if (!snapshot->enabled || !snapshot->field ||
        !isfinite(record->prepared_scale) ||
        record->prepared_scale == 0.0 ||
        !isfinite(record->verify_radius) ||
        record->verify_radius < 0.0 ||
        !isfinite(snapshot->verify_pix) ||
        !isfinite(snapshot->index_jitter) ||
        !isfinite(snapshot->logaccept) ||
        !isfinite(snapshot->logratio_bail_threshold)) {
        return -1;
    }
    global_candidate =
        packet->candidate_window_first + candidate_index;
    slot = &packet->slots[record->descriptor_index];
    if (slot->state != SOLVER_CODEKD_RESULT_READY ||
        !slot->hit_count ||
        slot->hit_first > packet->hit_count ||
        (size_t)slot->hit_count >
            packet->hit_count - slot->hit_first) {
        return -1;
    }
    slot_end = slot->hit_first + (size_t)slot->hit_count;
    if (global_candidate < slot->hit_first ||
        global_candidate >= slot_end ||
        packet->inds[global_candidate] != record->quadid) {
        return -1;
    }
    radius2 = square(record->verify_radius);
    if (memcmp(&radius2, &record->verify_radius2, sizeof(radius2))) {
        return -1;
    }

    memcpy(match, &snapshot->match_template, sizeof(*match));
    memcpy(&match->wcstan, &record->prepared_wcs,
           sizeof(match->wcstan));
    match->wcs_valid = TRUE;
    match->code_err = packet->sdists[global_candidate];
    match->scale = record->prepared_scale;
    match->parity = record->prepared_parity;
    match->quad_npeers = slot->hit_count;
    match->quadno = record->quadid;
    match->dimquads = packet->phase->descriptor.dimquads;
    for (star_index = 0;
         star_index < packet->phase->descriptor.dimquads;
         star_index++) {
        match->star[star_index] = record->stars[star_index];
        match->field[star_index] =
            record->prepared_fieldstars[star_index];
        match->ids[star_index] = 0;
    }
    memcpy(match->quadpix, record->prepared_fieldxy,
           (size_t)packet->phase->descriptor.dimquads * 2U *
               sizeof(*match->quadpix));
    memcpy(match->quadxyz, record->starxyz,
           (size_t)packet->phase->descriptor.dimquads * 3U *
               sizeof(*match->quadxyz));
    memcpy(match->center, record->verify_center,
           sizeof(match->center));
    match->radius = record->verify_radius;
    match->radius_deg = dist2deg(match->radius);
    match->indexid = snapshot->indexid;
    match->healpix = snapshot->healpix;
    match->hpnside = snapshot->hpnside;
    match->wcstan.imagew = input->phase->field_maxx;
    match->wcstan.imageh = input->phase->field_maxy;
    *verify_pix2 = square(snapshot->verify_pix) +
        square(snapshot->index_jitter / match->scale);
    *logaccept = snapshot->logaccept;
    return isfinite(*verify_pix2) ? 0 : -1;
}

static void solver_codekd_packet_discard_prepared_verification(
    solver_codekd_search_packet_t* packet) {
    size_t candidate_index;

    if (!packet || !packet->candidate_records) {
        return;
    }
    for (candidate_index = 0U;
         candidate_index < packet->candidate_window_count;
         candidate_index++) {
        solver_codekd_record_clear_prepared_verification(
            &packet->candidate_records[candidate_index]);
    }
    packet->verify_prepared_count = 0U;
    packet->verify_prepared_retained_bytes = 0U;
    packet->verify_prepared_transient_bytes = 0U;
}

int solver_codekd_packet_retired_verification_complete(
    solver_codekd_search_packet_t* packet) {
    size_t candidate_index;

    if (!packet || !packet->phase || !packet->candidate_records ||
        !packet->verify_plan_complete ||
        packet->verify_plan_end > packet->candidate_window_count) {
        return -1;
    }
    for (candidate_index = packet->verify_plan_first;
         candidate_index < packet->verify_plan_end;
         candidate_index++) {
        const solver_candidate_delivery_record_t* record =
            &packet->candidate_records[candidate_index];

        if (record->prepared_verification ||
            record->verification_score_ready ||
            record->prepared_score.complete ||
            record->prepared_score.theta ||
            record->prepared_score.allodds) {
            return -1;
        }
    }
    packet->verify_prepared_count = 0U;
    packet->verify_prepared_retained_bytes = 0U;
    packet->verify_prepared_transient_bytes = 0U;
    return 0;
}

/*
 * Consume only fully captured native query results and create a bounded
 * contiguous set of immutable, index-free score contexts. The packet claim
 * owns every mutation here; solver and reducer state remain owner-only.
 */
int solver_codekd_packet_prepare_verification_ready(
    const solver_codekd_packet_task_input_t* input,
    solver_codekd_search_packet_t* packet) {
    const solver_codekd_verification_snapshot_t* snapshot;
    size_t original_plan_end;
    size_t candidate_index;
    size_t verify_index;

    if (!input || !input->phase ||
        !packet || !packet->phase ||
        packet->phase != input->phase ||
        !packet->candidate_records ||
        packet->state !=
            SOLVER_CODEKD_PACKET_VERIFY_PREPARE_COMPUTE_READY ||
        !packet->verify_plan_complete ||
        packet->verify_plan_first !=
            packet->candidate_window_offset ||
        packet->verify_plan_end <= packet->verify_plan_first ||
        packet->verify_plan_end > packet->candidate_window_count ||
        packet->verify_prepared_count ||
        packet->verify_prepared_retained_bytes ||
        packet->verify_prepared_transient_bytes) {
        return -1;
    }
    snapshot = &input->phase->verification;
    if (!snapshot->enabled || !snapshot->field ||
        verify_datalog_enabled()) {
        packet->state = SOLVER_CODEKD_PACKET_RESULTS_READY;
        return 0;
    }
    original_plan_end = packet->verify_plan_end;
    verify_index = original_plan_end;
    for (candidate_index = packet->verify_plan_first;
         candidate_index < original_plan_end;
         candidate_index++) {
        if (packet->candidate_records[candidate_index].plan_action ==
                SOLVER_AB_CANDIDATE_VERIFY) {
            verify_index = candidate_index;
            break;
        }
    }
    if (verify_index == original_plan_end) {
        packet->state = SOLVER_CODEKD_PACKET_RESULTS_READY;
        return 0;
    }
    if (verify_index > packet->verify_plan_first) {
        packet->verify_plan_end = verify_index;
        packet->state = SOLVER_CODEKD_PACKET_RESULTS_READY;
        return 0;
    }
    for (candidate_index = verify_index;
         candidate_index < original_plan_end;
         candidate_index++) {
        solver_candidate_delivery_record_t* record =
            &packet->candidate_records[candidate_index];
        verify_prepared_hit_t* prepared = NULL;
        MatchObj match;
        size_t query_bytes;
        size_t remaining_query_bytes;
        size_t prepared_bytes;
        size_t score_bytes;
        size_t peak_bytes;
        size_t retained_bytes;
        size_t transient_bytes;
        size_t retained_total;
        size_t transient_max;
        double verify_pix2;
        double logaccept;

        if (record->plan_action != SOLVER_AB_CANDIDATE_VERIFY) {
            continue;
        }
        if (index_shard_worker_stop_requested()) {
            solver_codekd_packet_discard_prepared_verification(packet);
            packet->state = SOLVER_CODEKD_PACKET_STOPPED;
            return 2;
        }
        if (!record->candidate_prepared ||
            !record->verify_query ||
            !record->verify_query_captured) {
            record->verify_delivery_fallback = TRUE;
            goto close_prefix;
        }
        query_bytes = verify_index_query_bytes(record->verify_query);
        if (query_bytes == SIZE_MAX ||
            query_bytes > packet->verify_query_bytes ||
            solver_codekd_packet_build_verification_match(
                input, packet, record, candidate_index,
                &match, &verify_pix2, &logaccept) ||
            verify_prepare_hit_from_query(
                packet->phase->starkd,
                &record->verify_query,
                snapshot->index_cutnside,
                &match,
                NULL,
                snapshot->field,
                verify_pix2,
                snapshot->distractor_ratio,
                input->phase->field_maxx,
                input->phase->field_maxy,
                snapshot->logratio_bail_threshold,
                logaccept,
                snapshot->logratio_stoplooking,
                snapshot->distance_from_quad_bonus,
                FALSE,
                &prepared)) {
            verify_destroy_prepared_hit(prepared);
            record->verify_delivery_fallback = TRUE;
            goto close_prefix;
        }
        record->verify_query_captured = FALSE;
        remaining_query_bytes =
            packet->verify_query_bytes - query_bytes;
        prepared_bytes = verify_prepared_hit_bytes(prepared);
        score_bytes = verify_prepared_score_bytes(prepared);
        peak_bytes = verify_prepared_hit_peak_bytes(prepared);
        if (prepared_bytes == SIZE_MAX ||
            score_bytes == SIZE_MAX ||
            peak_bytes == SIZE_MAX ||
            prepared_bytes > SIZE_MAX - score_bytes ||
            peak_bytes < prepared_bytes + score_bytes ||
            remaining_query_bytes > packet->verify_query_budget ||
            packet->verify_prepared_retained_bytes >
                SIZE_MAX - prepared_bytes - score_bytes) {
            verify_destroy_prepared_hit(prepared);
            record->verify_delivery_fallback = TRUE;
            packet->verify_query_bytes = remaining_query_bytes;
            goto close_prefix;
        }
        retained_bytes = prepared_bytes + score_bytes;
        transient_bytes = peak_bytes - retained_bytes;
        retained_total =
            packet->verify_prepared_retained_bytes +
            retained_bytes;
        transient_max = MAX(
            packet->verify_prepared_transient_bytes,
            transient_bytes);
        if (retained_total >
                packet->verify_query_budget -
                    remaining_query_bytes ||
            transient_max >
                packet->verify_query_budget -
                    remaining_query_bytes -
                    retained_total) {
            verify_destroy_prepared_hit(prepared);
            record->verify_delivery_fallback = TRUE;
            packet->verify_query_bytes = remaining_query_bytes;
            goto close_prefix;
        }
        packet->verify_query_bytes = remaining_query_bytes;
        record->prepared_verification = prepared;
        packet->verify_prepared_retained_bytes =
            retained_total;
        packet->verify_prepared_transient_bytes =
            transient_max;
        packet->verify_prepared_count++;
        continue;

close_prefix:
        if (!packet->verify_prepared_count) {
            packet->verify_plan_end = candidate_index + 1U;
            solver_codekd_packet_discard_prepared_verification(packet);
            packet->state = SOLVER_CODEKD_PACKET_RESULTS_READY;
            return 0;
        }
        packet->verify_plan_end = candidate_index;
        packet->verify_topology_end = candidate_index;
        break;
    }

    if (!packet->verify_prepared_count ||
        packet->verify_plan_end <= packet->verify_plan_first) {
        solver_codekd_packet_discard_prepared_verification(packet);
        packet->state = SOLVER_CODEKD_PACKET_RESULTS_READY;
        return 0;
    }
    packet->state =
        SOLVER_CODEKD_PACKET_VERIFY_SCORE_COMPUTE_READY;
    return 1;
}

index_shard_helper_task_status_t
solver_codekd_packet_score_verification_ready(
    solver_codekd_search_packet_t* packet) {
    index_shard_helper_task_status_t status =
        INDEX_SHARD_HELPER_TASK_OK;
    size_t candidate_index;
    size_t completed = 0U;

    if (!packet || !packet->phase || !packet->candidate_records ||
        packet->state !=
            SOLVER_CODEKD_PACKET_VERIFY_SCORE_COMPUTE_READY ||
        !packet->verify_plan_complete ||
        packet->verify_plan_first !=
            packet->candidate_window_offset ||
        packet->verify_plan_first == SIZE_MAX ||
        packet->verify_plan_end <= packet->verify_plan_first ||
        packet->verify_plan_end > packet->candidate_window_count ||
        !packet->verify_prepared_count ||
        !packet->verify_prepared_retained_bytes) {
        return INDEX_SHARD_HELPER_TASK_ERROR;
    }
    for (candidate_index = packet->verify_plan_first;
         candidate_index < packet->verify_plan_end;
         candidate_index++) {
        solver_candidate_delivery_record_t* record =
            &packet->candidate_records[candidate_index];
        if (!record->prepared_verification) {
            continue;
        }
        if (record->plan_action != SOLVER_AB_CANDIDATE_VERIFY ||
            record->verification_score_ready) {
            status = INDEX_SHARD_HELPER_TASK_ERROR;
            goto done;
        }
        if (index_shard_worker_stop_requested()) {
            solver_codekd_packet_discard_prepared_verification(packet);
            packet->state = SOLVER_CODEKD_PACKET_STOPPED;
            status = INDEX_SHARD_HELPER_TASK_STOPPED;
            goto done;
        }
        if (verify_score_prepared_hit(
                record->prepared_verification,
                &record->prepared_score)) {
            solver_codekd_packet_discard_prepared_verification(packet);
            packet->state = SOLVER_CODEKD_PACKET_RESULTS_READY;
            goto done;
        }
        record->verification_score_ready = TRUE;
        completed++;
    }
    if (completed != packet->verify_prepared_count) {
        status = INDEX_SHARD_HELPER_TASK_ERROR;
        goto done;
    }
    packet->state = SOLVER_CODEKD_PACKET_RESULTS_READY;

done:
    return status;
}
