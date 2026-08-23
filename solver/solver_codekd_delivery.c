/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */
/* CodeKD descriptor generation and bounded payload delivery. */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "solver.h"
#include "verify.h"
#include "fitsbin.h"
#include "fit-wcs.h"
#include "log.h"
#include "mathutil.h"
#include "quad-utils.h"
#include "tic.h"
#include "index_shard_internal.h"
#include "solver_codekd_internal.h"
#include "solver_inline_internal.h"
#include "solver_hypothesis_internal.h"
#include "../libkd/kdtree_prefetch_internal.h"
/*
 * Return zero when the bounded output arena is ready, one for an exact native
 * fallback, and minus one for an invalid packet request.
 */
int solver_codekd_search_packet_prepare(
    const solver_t* solver,
    solver_codekd_search_packet_t* packet,
    solver_ab_descriptor_output_t* descriptors,
    const solver_codekd_phase_context_t* phase,
    size_t candidate_budget_bytes,
    unsigned long long sequence) {
    size_t metadata_bytes;
    size_t hit_bytes;

    if (!solver || !packet || !descriptors || !phase ||
        !phase->tree || !phase->quads || !phase->starkd ||
        !phase->starkd->tree ||
        phase->descriptor.dimquads <= 0 ||
        phase->descriptor.dimquads > DQMAX ||
        SOLVER_AB_DESCRIPTOR_CAPACITY >
            SIZE_MAX / sizeof(*packet->slots)) {
        return -1;
    }
#ifdef SOLVER_CODEKD_TEST_FORCE_PACKET_ALLOC_FAILURE
    if (solver->profile.ab_helper_tasks &&
        !solver->profile.allocation_failures) {
        return 1;
    }
#endif
    memset(packet, 0, sizeof(*packet));
    metadata_bytes =
        SOLVER_AB_DESCRIPTOR_CAPACITY * sizeof(*packet->slots);
    if (metadata_bytes >= SOLVER_CODEKD_PACKET_RESULT_LIMIT_BYTES) {
        return 1;
    }
    hit_bytes =
        SOLVER_CODEKD_PACKET_RESULT_LIMIT_BYTES - metadata_bytes;
    packet->hit_capacity =
        hit_bytes / (sizeof(*packet->inds) + sizeof(*packet->sdists));
    if (!packet->hit_capacity ||
        packet->hit_capacity > SIZE_MAX / sizeof(*packet->inds) ||
        packet->hit_capacity > SIZE_MAX / sizeof(*packet->sdists)) {
        return 1;
    }

    packet->slots = calloc(
        SOLVER_AB_DESCRIPTOR_CAPACITY, sizeof(*packet->slots));
    packet->inds = malloc(
        packet->hit_capacity * sizeof(*packet->inds));
    packet->sdists = malloc(
        packet->hit_capacity * sizeof(*packet->sdists));
    if (!packet->slots || !packet->inds || !packet->sdists ||
        solver_codekd_page_workspace_create(
            &packet->page_workspace)) {
        (void)solver_codekd_search_packet_cleanup(packet);
        return 1;
    }

    if (candidate_budget_bytes >=
        sizeof(*packet->candidate_records)) {
        packet->candidate_capacity = MIN(
            packet->hit_capacity,
            candidate_budget_bytes /
                sizeof(*packet->candidate_records));
        /*
         * candidate_records is a rolling scratch window, not storage for the
         * complete result stream. The owner retires this bounded prefix
         * before the same logical packet submits its next payload window.
         */
        packet->candidate_capacity = MIN(
            packet->candidate_capacity,
            (size_t)SOLVER_CANDIDATE_DELIVERY_LIMIT);
        if (packet->candidate_capacity >
            SIZE_MAX / sizeof(*packet->candidate_records)) {
            packet->candidate_capacity = 0U;
        }
        if (packet->candidate_capacity) {
            packet->candidate_records = calloc(
                packet->candidate_capacity,
                sizeof(*packet->candidate_records));
            if (!packet->candidate_records) {
                packet->candidate_capacity = 0U;
            } else {
                size_t record_bytes =
                    packet->candidate_capacity *
                        sizeof(*packet->candidate_records);

                packet->verify_query_budget =
                    candidate_budget_bytes > record_bytes
                    ? candidate_budget_bytes - record_bytes
                    : 0U;
            }
        }
    }

    packet->descriptors = descriptors;
    packet->phase = phase;
    packet->candidate_star_delivery_disabled =
        phase->starkd->tree->perm &&
        phase->starkd->inverse_perm == NULL;
    packet->sequence = sequence;
    packet->state = SOLVER_CODEKD_PACKET_ALLOCATED;
    return 0;
}

index_shard_helper_task_status_t
solver_codekd_packet_generate_descriptors(
    const void* input_bytes,
    size_t input_size,
    void* output_bytes,
    size_t output_size) {
    const solver_codekd_packet_task_input_t* input = input_bytes;
    solver_codekd_search_packet_t* packet = output_bytes;
    solver_ab_descriptor_task_input_t descriptor_input;
    index_shard_helper_task_status_t descriptor_status;

    if (!input || input_size != sizeof(*input) ||
        !input->phase ||
        !packet || output_size != sizeof(*packet) ||
        packet->phase != input->phase ||
        !input->phase->tree || !input->phase->quads ||
        !input->phase->starkd ||
        !packet->descriptors || !packet->slots ||
        !packet->inds || !packet->sdists ||
        packet->sequence != input->combination_first ||
        packet->state != SOLVER_CODEKD_PACKET_ALLOCATED) {
        return INDEX_SHARD_HELPER_TASK_ERROR;
    }

    descriptor_input.phase = &input->phase->descriptor;
    descriptor_input.combination_first = input->combination_first;
    descriptor_input.combination_end = input->combination_end;
    descriptor_status = solver_ab_descriptor_helper_execute(
        &descriptor_input,
        sizeof(descriptor_input),
        packet->descriptors,
        sizeof(*packet->descriptors));
    if (descriptor_status != INDEX_SHARD_HELPER_TASK_OK) {
        packet->state =
            descriptor_status == INDEX_SHARD_HELPER_TASK_STOPPED
                ? SOLVER_CODEKD_PACKET_STOPPED
                : SOLVER_CODEKD_PACKET_FAILED;
        return descriptor_status;
    }
    if (packet->descriptors->descriptor_count >
        SOLVER_AB_DESCRIPTOR_CAPACITY) {
        packet->state = SOLVER_CODEKD_PACKET_FAILED;
        return INDEX_SHARD_HELPER_TASK_ERROR;
    }
    packet->count = packet->descriptors->descriptor_count;
    packet->next_descriptor = 0U;
    packet->state = SOLVER_CODEKD_PACKET_DESCRIPTORS_READY;
    return INDEX_SHARD_HELPER_TASK_OK;
}

static int solver_codekd_packet_mark_plan_owner_replay(
    solver_codekd_search_packet_t* packet) {
    size_t descriptor_index;

    if (!packet || !packet->plan_complete ||
        packet->plan_first >= packet->plan_end ||
        packet->plan_end > packet->count) {
        return -1;
    }
    for (descriptor_index = packet->plan_first;
         descriptor_index < packet->plan_end;
         descriptor_index++) {
        if (packet->slots[descriptor_index].state !=
            SOLVER_CODEKD_RESULT_UNUSED) {
            return -1;
        }
        packet->slots[descriptor_index].state =
            SOLVER_CODEKD_RESULT_OWNER_REPLAY;
    }
    solver_codekd_packet_reset_page_plan(packet);
    packet->state = SOLVER_CODEKD_PACKET_DESCRIPTORS_READY;
    return 0;
}

/*
 * A permanent pre-submission refusal invalidates the current sealed plan and
 * makes another plan attempt uneconomic. Preserve the exact logical work by
 * replaying the planned prefix and every still-unplanned descriptor at owner
 * retirement.
 */
static int solver_codekd_packet_mark_outstanding_owner_replay(
    solver_codekd_search_packet_t* packet) {
    size_t descriptor_index;

    if (!packet || !packet->plan_complete ||
        packet->state != SOLVER_CODEKD_PACKET_PAGE_PLAN_COMPLETE ||
        packet->plan_first >= packet->plan_end ||
        packet->plan_end > packet->next_descriptor ||
        packet->next_descriptor > packet->count) {
        return -1;
    }
    for (descriptor_index = packet->plan_first;
         descriptor_index < packet->plan_end;
         descriptor_index++) {
        if (packet->slots[descriptor_index].state !=
            SOLVER_CODEKD_RESULT_UNUSED) {
            return -1;
        }
    }
    for (descriptor_index = packet->plan_end;
         descriptor_index < packet->count;
         descriptor_index++) {
        if (packet->slots[descriptor_index].state !=
                SOLVER_CODEKD_RESULT_UNUSED &&
            packet->slots[descriptor_index].state !=
                SOLVER_CODEKD_RESULT_OWNER_REPLAY) {
            return -1;
        }
    }
    for (descriptor_index = packet->plan_first;
         descriptor_index < packet->count;
         descriptor_index++) {
        if (packet->slots[descriptor_index].state ==
            SOLVER_CODEKD_RESULT_UNUSED) {
            packet->slots[descriptor_index].state =
                SOLVER_CODEKD_RESULT_OWNER_REPLAY;
        }
    }
    packet->next_descriptor = packet->count;
    packet->pending_descriptor_plan = FALSE;
    solver_codekd_packet_reset_page_plan(packet);
    packet->state = SOLVER_CODEKD_PACKET_DESCRIPTORS_READY;
    return 0;
}

/* Return one when submitted, zero on bounded capacity, and minus one when
 * the complete packet must use exact owner replay. */
int solver_codekd_packet_submit_pages(
    solver_codekd_search_packet_t* packet) {
    fitsbin_t* source;
    int submit_status;

    if (!packet || !packet->phase ||
        !packet->phase->tree || !packet->phase->tree->io ||
        !packet->phase->tree->io_is_fitsbin || !packet->page_workspace ||
        packet->delivery_ticket ||
        packet->state != SOLVER_CODEKD_PACKET_PAGE_PLAN_COMPLETE ||
        !packet->plan_complete ||
        packet->plan_first >= packet->plan_end ||
        packet->plan_end > packet->next_descriptor ||
        packet->next_descriptor > packet->count ||
        !packet->plan_range_count ||
        packet->plan_range_count >
            packet->page_workspace->sealed_range_capacity) {
        return -1;
    }
    if (index_shard_worker_stop_requested()) {
        packet->state = SOLVER_CODEKD_PACKET_STOPPED;
        return -1;
    }
    source = (fitsbin_t*)packet->phase->tree->io;
    errno = 0;
    submit_status = fitsbin_prefetch_ranges_submit(
        source,
        packet->page_workspace->sealed_ranges,
        packet->plan_range_count,
        SOLVER_CODEKD_DELIVERY_BUDGET_BYTES,
        &packet->delivery_ticket);
    if (submit_status == FITSBIN_PAYLOAD_IO_SUBMIT_READY &&
        !packet->delivery_ticket) {
        packet->state = SOLVER_CODEKD_PACKET_COMPUTE_READY;
        return 2;
    }
    if (submit_status > 0 && packet->delivery_ticket) {
        return 1;
    }
    packet->delivery_ticket = NULL;
    if (!submit_status && errno == EAGAIN) {
        return 0;
    }
    if (index_shard_worker_stop_requested()) {
        packet->state = SOLVER_CODEKD_PACKET_STOPPED;
        return -1;
    }
    if (solver_codekd_packet_mark_outstanding_owner_replay(packet)) {
        packet->state = SOLVER_CODEKD_PACKET_FAILED;
    }
    return -1;
}

static void solver_codekd_packet_reset_window_state(
    solver_codekd_search_packet_t* packet) {
    packet->candidate_window_offset = 0U;
    packet->candidate_star_count = 0U;
    packet->candidate_quad_ready_count = 0U;
    packet->candidate_star_ready_count = 0U;
    packet->candidate_verify_query_count = 0U;
    packet->verify_plan_first = 0U;
    packet->verify_plan_end = 0U;
    packet->verify_topology_end = 0U;
    packet->verify_plan_range_count = 0U;
    packet->verify_query_bytes = 0U;
    packet->verify_prepared_count = 0U;
    packet->verify_prepared_retained_bytes = 0U;
    packet->verify_prepared_transient_bytes = 0U;
    packet->verify_sweep_range_count = 0U;
    packet->verify_sweep_aligned_bytes = 0U;
    packet->verify_pending_query_index = 0U;
    packet->verify_plan_complete = FALSE;
    packet->verify_sweep_plan_complete = FALSE;
    packet->verify_pending_query_plan = FALSE;
    packet->verification_delivery_disabled = FALSE;
}

int solver_codekd_packet_begin_candidate_window(
    solver_codekd_search_packet_t* packet) {
    size_t descriptor_index;
    anbool found = FALSE;

    if (!packet || !packet->candidate_records ||
        !packet->candidate_capacity || !packet->candidate_count ||
        packet->candidate_cursor >= packet->candidate_count ||
        packet->candidate_window_count ||
        packet->candidate_window_offset) {
        return -1;
    }
    for (descriptor_index = packet->retire_descriptor;
         descriptor_index < packet->count;
         descriptor_index++) {
        const solver_codekd_result_slot_t* slot =
            &packet->slots[descriptor_index];
        size_t slot_end;

        if (slot->state != SOLVER_CODEKD_RESULT_READY ||
            !slot->hit_count ||
            slot->hit_first > packet->hit_count ||
            (size_t)slot->hit_count >
                packet->hit_count - slot->hit_first) {
            continue;
        }
        slot_end = slot->hit_first + (size_t)slot->hit_count;
        if (packet->candidate_cursor >= slot->hit_first &&
            packet->candidate_cursor < slot_end) {
            found = TRUE;
            break;
        }
    }
    if (!found) {
        return -1;
    }
    packet->candidate_window_first = packet->candidate_cursor;
    packet->candidate_window_count = MIN(
        MIN(packet->candidate_capacity,
            (size_t)SOLVER_CANDIDATE_DELIVERY_LIMIT),
        packet->candidate_count - packet->candidate_cursor);
    if (!packet->candidate_window_count) {
        return -1;
    }
    solver_codekd_packet_reset_window_state(packet);
    packet->candidate_delivery_windows++;
    packet->state = SOLVER_CODEKD_PACKET_QUAD_SUBMIT_READY;
    return 0;
}

void solver_codekd_packet_clear_candidate_window(
    solver_codekd_search_packet_t* packet) {
    size_t candidate_index;

    if (!packet) {
        return;
    }
    assert(!packet->delivery_ticket);
    for (candidate_index = 0U;
         candidate_index < packet->candidate_window_count;
         candidate_index++) {
        solver_codekd_record_clear_verification_speculation(
            &packet->candidate_records[candidate_index]);
    }
    packet->candidate_window_first = packet->candidate_cursor;
    packet->candidate_window_count = 0U;
    solver_codekd_packet_reset_window_state(packet);
}

/*
 * Build verification queries from already copied candidate data. The record
 * also retains the exact native gates and TAN result so ordered retirement
 * can reuse the calculation after validating the complete immutable input.
 */
static int solver_codekd_packet_build_verify_queries(
    solver_codekd_search_packet_t* packet,
    const solver_codekd_packet_task_input_t* input) {
    const solver_field_geometry_t* geometry;
    size_t candidate_index;

    if (!packet || !input || !input->phase ||
        packet->phase != input->phase ||
        input->phase->use_radec ||
        !packet->candidate_records ||
        !packet->candidate_window_count ||
        packet->candidate_star_ready_count !=
            packet->candidate_window_count ||
        packet->phase->descriptor.dimquads <= 0 ||
        packet->phase->descriptor.dimquads > DQMAX) {
        return -1;
    }
    geometry = input->phase->descriptor.field_geometry;
    if (!geometry || !geometry->fieldxy ||
        geometry->numxy <= 0) {
        return -1;
    }
    packet->candidate_verify_query_count = 0U;

    for (candidate_index = 0U;
         candidate_index < packet->candidate_window_count;
         candidate_index++) {
        solver_candidate_delivery_record_t* record =
            &packet->candidate_records[candidate_index];
        const solver_ab_descriptor_t* descriptor;
        double field_xy[DQMAX * 2];
        double corner[3];
        double scale;
        double arcsecperpix;
        double abscale;
        double cx;
        double cy;
        double radius;
        tan_t wcs;
        int star_index;

        record->plan_action = SOLVER_AB_CANDIDATE_SCALE_SKIP;
        record->candidate_prepared = FALSE;
        memset(record->verify_center, 0, sizeof(record->verify_center));
        record->verify_radius = 0.0;
        record->verify_radius2 = 0.0;
        if (record->descriptor_index >= packet->count) {
            return -1;
        }
        descriptor = &packet->descriptors->descriptors[
            record->descriptor_index];
        for (star_index = 0;
             star_index < packet->phase->descriptor.dimquads;
             star_index++) {
            int field_star = descriptor->stars[star_index];

            if (field_star < 0 ||
                field_star >= geometry->numxy) {
                return -1;
            }
            setx(
                field_xy,
                star_index,
                starxy_getx(geometry->fieldxy, field_star));
            sety(
                field_xy,
                star_index,
                starxy_gety(geometry->fieldxy, field_star));
            record->prepared_fieldstars[star_index] = field_star;
        }
        memcpy(
            record->prepared_fieldxy,
            field_xy,
            (size_t)packet->phase->descriptor.dimquads * 2U * sizeof(*field_xy));
        record->prepared_parity = descriptor->current_parity;

        abscale = square(distsq2rad(distsq(
            record->starxyz, record->starxyz + 3, 3))) /
            distsq(field_xy, field_xy + 2, 2);
        if (abscale > input->phase->abscale_high ||
            abscale < input->phase->abscale_low) {
            record->plan_action =
                SOLVER_AB_CANDIDATE_ABSCALE_SKIP;
            record->candidate_prepared = TRUE;
            continue;
        }
        if (fit_tan_wcs(
                record->starxyz,
                field_xy,
                packet->phase->descriptor.dimquads,
                &wcs,
                &scale)) {
            record->plan_action =
                SOLVER_AB_CANDIDATE_BAD_QUAD;
            record->candidate_prepared = TRUE;
            continue;
        }
        arcsecperpix = scale * 3600.0;
        memcpy(&record->prepared_wcs, &wcs, sizeof(wcs));
        record->prepared_scale = arcsecperpix;
        record->candidate_prepared = TRUE;
        if (arcsecperpix > input->phase->funits_upper ||
            arcsecperpix < input->phase->funits_lower) {
            continue;
        }

        record->plan_action = SOLVER_AB_CANDIDATE_VERIFY;
        cx = 0.5 * (input->phase->field_minx + input->phase->field_maxx);
        cy = 0.5 * (input->phase->field_miny + input->phase->field_maxy);
        tan_pixelxy2xyzarr(
            &wcs, cx, cy, record->verify_center);
        tan_pixelxy2xyzarr(
            &wcs, input->phase->field_minx, input->phase->field_miny, corner);
        radius = sqrt(distsq(
            record->verify_center, corner, 3));
        record->verify_radius = radius;
        record->verify_radius2 = square(radius);
        if (!isfinite(record->verify_radius2) ||
            record->verify_radius2 < 0.0) {
            record->verify_radius = 0.0;
            record->verify_radius2 = 0.0;
            record->verify_delivery_fallback = TRUE;
            continue;
        }
        packet->candidate_verify_query_count++;
    }
    return 0;
}

/*
 * Complete the CodeKD phase through one central transition. The first
 * bounded Quad/A/B window becomes independently deliverable.
 */
int solver_codekd_packet_finish_codekd(
    solver_codekd_search_packet_t* packet) {
    if (!packet || !packet->phase ||
        (packet->state != SOLVER_CODEKD_PACKET_DESCRIPTORS_READY &&
         packet->state != SOLVER_CODEKD_PACKET_RESULTS_READY) ||
        packet->next_descriptor != packet->count ||
        packet->plan_complete || packet->delivery_ticket) {
        return -1;
    }
    if (packet->candidate_count) {
        return packet->state == SOLVER_CODEKD_PACKET_RESULTS_READY
            ? 0
            : -1;
    }
    if (!packet->hit_count || !packet->candidate_records ||
        !packet->candidate_capacity || packet->phase->use_radec ||
        !packet->phase->quads || !packet->phase->starkd ||
        packet->phase->descriptor.dimquads <= 0 ||
        packet->phase->descriptor.dimquads > DQMAX) {
        packet->state = SOLVER_CODEKD_PACKET_RESULTS_READY;
        return 0;
    }

    packet->candidate_count = packet->hit_count;
    packet->candidate_cursor = 0U;
    packet->retire_descriptor = 0U;
    packet->retire_hit_offset = 0U;
    packet->retire_descriptor_started = FALSE;
    return solver_codekd_packet_begin_candidate_window(packet);
}

/*
 * Submit one bounded post-CodeKD payload stage. A permanent refusal preserves
 * the copied CodeKD results and falls back only candidate resolution.
 */
int solver_codekd_packet_submit_candidate_pages(
    solver_codekd_search_packet_t* packet) {
    int submit_status;

    if (!packet || !packet->phase ||
        packet->delivery_ticket ||
        !packet->candidate_records || !packet->candidate_count ||
        packet->candidate_cursor >= packet->candidate_count ||
        packet->candidate_window_first != packet->candidate_cursor ||
        packet->candidate_window_offset ||
        !packet->candidate_window_count ||
        packet->candidate_window_count >
            SOLVER_CANDIDATE_DELIVERY_LIMIT ||
        packet->candidate_window_count >
            packet->candidate_capacity ||
        packet->candidate_window_count >
            packet->candidate_count -
                packet->candidate_window_first) {
        return -1;
    }
    if (index_shard_worker_stop_requested()) {
        packet->state = SOLVER_CODEKD_PACKET_STOPPED;
        return -1;
    }

    errno = 0;
    if (packet->state == SOLVER_CODEKD_PACKET_QUAD_SUBMIT_READY) {
        if (packet->candidate_quad_delivery_disabled) {
            packet->state = SOLVER_CODEKD_PACKET_RESULTS_READY;
            return -1;
        }
        if (!packet->phase->quads || !packet->phase->quads->fb) {
            packet->candidate_quad_delivery_disabled = TRUE;
            packet->candidate_star_delivery_disabled = TRUE;
            packet->state = SOLVER_CODEKD_PACKET_RESULTS_READY;
            return -1;
        }
        submit_status = quadfile_prefetch_stars_submit(
            packet->phase->quads,
            packet->inds + packet->candidate_window_first,
            (int)packet->candidate_window_count,
            &packet->delivery_ticket);
        if (submit_status == FITSBIN_PAYLOAD_IO_SUBMIT_READY &&
            !packet->delivery_ticket) {
            packet->state = SOLVER_CODEKD_PACKET_QUAD_COMPUTE_READY;
            return 2;
        }
        if (submit_status > 0 && packet->delivery_ticket) {
            packet->state = SOLVER_CODEKD_PACKET_QUAD_IO_SUBMITTED;
            return 1;
        }
        packet->delivery_ticket = NULL;
        if (!submit_status && errno == EAGAIN) {
            return 0;
        }
        packet->candidate_quad_delivery_disabled = TRUE;
        packet->candidate_star_delivery_disabled = TRUE;
        packet->state = SOLVER_CODEKD_PACKET_RESULTS_READY;
        return -1;
    }

    if (packet->state != SOLVER_CODEKD_PACKET_STAR_SUBMIT_READY ||
        packet->candidate_star_delivery_disabled ||
        !packet->phase->starkd || !packet->phase->starkd->tree ||
        !packet->phase->starkd->tree->io ||
        !packet->phase->starkd->tree->io_is_fitsbin ||
        !packet->candidate_star_count ||
        packet->candidate_star_count > SOLVER_CANDIDATE_STAR_LIMIT) {
        packet->candidate_star_delivery_disabled = TRUE;
        packet->state = SOLVER_CODEKD_PACKET_RESULTS_READY;
        return -1;
    }
    submit_status = startree_prefetch_stars_ready_submit(
        packet->phase->starkd,
        packet->candidate_starids,
        (int)packet->candidate_star_count,
        &packet->delivery_ticket);
    if (submit_status == FITSBIN_PAYLOAD_IO_SUBMIT_READY &&
        !packet->delivery_ticket) {
        packet->state = SOLVER_CODEKD_PACKET_STAR_COMPUTE_READY;
        return 2;
    }
    if (submit_status > 0 && packet->delivery_ticket) {
        packet->state = SOLVER_CODEKD_PACKET_STAR_IO_SUBMITTED;
        return 1;
    }
    packet->delivery_ticket = NULL;
    if (!submit_status && errno == EAGAIN) {
        return 0;
    }
    packet->candidate_star_delivery_disabled = TRUE;
    packet->state = SOLVER_CODEKD_PACKET_RESULTS_READY;
    return -1;
}

int solver_codekd_packet_submit_verification_pages(
    solver_codekd_search_packet_t* packet) {
    fitsbin_t* source;
    size_t candidate_index;
    anbool has_query = FALSE;
    int submit_status;

    if (!packet || !packet->phase || packet->delivery_ticket ||
        !packet->candidate_records ||
        packet->state !=
            SOLVER_CODEKD_PACKET_VERIFY_SUBMIT_READY ||
        packet->verification_delivery_disabled ||
        packet->verify_plan_complete ||
        packet->candidate_window_offset >=
            packet->candidate_window_count ||
        packet->candidate_star_ready_count !=
            packet->candidate_window_count ||
        !packet->phase->starkd || !packet->phase->starkd->tree ||
        !packet->phase->starkd->tree->io ||
        !packet->phase->starkd->tree->io_is_fitsbin) {
        return -1;
    }
    if (index_shard_worker_stop_requested()) {
        packet->state = SOLVER_CODEKD_PACKET_STOPPED;
        return -1;
    }
    for (candidate_index = packet->candidate_window_offset;
         candidate_index < packet->candidate_window_count;
         candidate_index++) {
        const solver_candidate_delivery_record_t* record =
            &packet->candidate_records[candidate_index];

        if (record->verify_delivery_fallback) {
            solver_codekd_packet_disable_verification_delivery(
                packet);
            return -1;
        }
        if (record->plan_action !=
            SOLVER_AB_CANDIDATE_VERIFY) {
            continue;
        }
        has_query = TRUE;
        break;
    }
    if (!has_query) {
        packet->verify_plan_first =
            packet->candidate_window_offset;
        packet->verify_plan_end =
            packet->candidate_window_count;
        packet->verify_topology_end =
            packet->candidate_window_count;
        packet->verify_plan_complete = TRUE;
        packet->state =
            SOLVER_CODEKD_PACKET_VERIFY_COMPUTE_READY;
        return 2;
    }

    source = (fitsbin_t*)packet->phase->starkd->tree->io;
    packet->state = SOLVER_CODEKD_PACKET_VERIFY_IO_SUBMITTED;
    errno = 0;
    submit_status = fitsbin_prefetch_ranges_planned_submit(
        source,
        solver_codekd_packet_plan_verification_pages,
        packet,
        SOLVER_CODEKD_DELIVERY_BUDGET_BYTES,
        &packet->delivery_ticket);
    if (submit_status > 0 && packet->delivery_ticket) {
        return 1;
    }
    packet->delivery_ticket = NULL;
    if (!submit_status && errno == EAGAIN) {
        packet->state =
            SOLVER_CODEKD_PACKET_VERIFY_SUBMIT_READY;
        return 0;
    }
    solver_codekd_packet_disable_verification_delivery(packet);
    return -1;
}

int solver_codekd_packet_submit_sweep_pages(
    solver_codekd_search_packet_t* packet) {
    fitsbin_t* source;
    int submit_status;

    if (!packet || !packet->phase || packet->delivery_ticket ||
        !packet->phase->starkd ||
        !packet->phase->starkd->tree ||
        !packet->phase->starkd->tree->io ||
        !packet->phase->starkd->tree->io_is_fitsbin ||
        !packet->page_workspace ||
        packet->state !=
            SOLVER_CODEKD_PACKET_VERIFY_SWEEP_SUBMIT_READY ||
        !packet->verify_plan_complete ||
        !packet->verify_sweep_plan_complete ||
        !packet->verify_sweep_range_count ||
        packet->verify_sweep_range_count >
            packet->page_workspace->sealed_range_capacity ||
        !packet->verify_sweep_aligned_bytes ||
        packet->verify_sweep_aligned_bytes >
            SOLVER_CODEKD_DELIVERY_BUDGET_BYTES) {
        return -1;
    }
    if (index_shard_worker_stop_requested()) {
        packet->state = SOLVER_CODEKD_PACKET_STOPPED;
        return -1;
    }

    source = (fitsbin_t*)packet->phase->starkd->tree->io;
    errno = 0;
    submit_status = fitsbin_prefetch_ranges_submit(
        source,
        packet->page_workspace->sealed_ranges,
        packet->verify_sweep_range_count,
        SOLVER_CODEKD_DELIVERY_BUDGET_BYTES,
        &packet->delivery_ticket);
    if (submit_status == FITSBIN_PAYLOAD_IO_SUBMIT_READY &&
        !packet->delivery_ticket) {
        packet->state =
            SOLVER_CODEKD_PACKET_VERIFY_SWEEP_COMPUTE_READY;
        return 2;
    }
    if (submit_status > 0 && packet->delivery_ticket) {
        packet->state =
            SOLVER_CODEKD_PACKET_VERIFY_SWEEP_IO_SUBMITTED;
        return 1;
    }
    packet->delivery_ticket = NULL;
    if (!submit_status && errno == EAGAIN) {
        return 0;
    }
    packet->state = SOLVER_CODEKD_PACKET_VERIFY_COMPUTE_READY;
    return 2;
}

/* Return zero while pending, one when compute-ready, two when replanned work
 * remains, three when the logical packet is complete, and minus one on error
 * or cooperative stop. */
int solver_codekd_packet_collect_pages(
    solver_codekd_search_packet_t* packet) {
    solver_codekd_search_packet_state_t io_state;
    int ticket_result = 0;
    int ticket_errno;
    int poll_status;

    if (!packet || !packet->delivery_ticket) {
        return -1;
    }
    errno = 0;
    poll_status = fitsbin_payload_io_ticket_poll_and_destroy(
        &packet->delivery_ticket,
        &ticket_result);
    ticket_errno = errno;
    if (!poll_status) {
        return 0;
    }
    if (poll_status < 0) {
        int drain_status =
            fitsbin_payload_io_ticket_drain_and_destroy(
                &packet->delivery_ticket,
                &ticket_result);

        if (drain_status != 1 || packet->delivery_ticket) {
            logerr("[solver] payload ticket ownership drain "
                   "failed errno=%i state=%i\n",
                   errno,
                   (int)packet->state);
        }
        logerr("[solver] payload ticket ownership transfer "
               "failed errno=%i state=%i\n",
               ticket_errno,
               (int)packet->state);
        packet->state = SOLVER_CODEKD_PACKET_FAILED;
        return -1;
    }
    /*
     * Verification planned-ticket output is owned by the I/O lane until
     * terminal publication. Poll's payload_io mutex edge makes those state
     * writes visible here. Initial CodeKD tickets use an immutable sealed
     * plan, so their packet remains PAGE_PLAN_COMPLETE while queued.
     */
    io_state = packet->state;
    assert(!packet->delivery_ticket);
    if ((ticket_result == 0 && ticket_errno == ECANCELED) ||
        index_shard_worker_stop_requested()) {
        packet->state = SOLVER_CODEKD_PACKET_STOPPED;
        return -1;
    }
    if (ticket_result > 0 &&
        io_state == SOLVER_CODEKD_PACKET_QUAD_IO_SUBMITTED) {
        packet->state = SOLVER_CODEKD_PACKET_QUAD_COMPUTE_READY;
        return 1;
    }
    if (ticket_result > 0 &&
        io_state == SOLVER_CODEKD_PACKET_STAR_IO_SUBMITTED) {
        packet->state = SOLVER_CODEKD_PACKET_STAR_COMPUTE_READY;
        return 1;
    }
    if (ticket_result > 0 &&
        io_state ==
            SOLVER_CODEKD_PACKET_VERIFY_PAGE_PLAN_COMPLETE &&
        packet->verify_plan_complete &&
        packet->verify_plan_first ==
            packet->candidate_window_offset &&
        packet->verify_plan_end >
            packet->verify_plan_first &&
            packet->verify_plan_end <=
            packet->candidate_window_count) {
        packet->state =
            SOLVER_CODEKD_PACKET_VERIFY_QUERY_COMPUTE_READY;
        return 1;
    }
    if (ticket_result > 0 &&
        io_state ==
            SOLVER_CODEKD_PACKET_VERIFY_SWEEP_IO_SUBMITTED &&
        packet->verify_plan_complete &&
        packet->verify_sweep_plan_complete &&
        packet->verify_sweep_range_count) {
        packet->state =
            SOLVER_CODEKD_PACKET_VERIFY_SWEEP_COMPUTE_READY;
        return 1;
    }
    if (ticket_result < 0 &&
        io_state ==
            SOLVER_CODEKD_PACKET_VERIFY_SWEEP_IO_SUBMITTED) {
        packet->state =
            SOLVER_CODEKD_PACKET_VERIFY_COMPUTE_READY;
        return 1;
    }
    if (ticket_result < 0 &&
        (io_state == SOLVER_CODEKD_PACKET_QUAD_IO_SUBMITTED ||
         io_state == SOLVER_CODEKD_PACKET_STAR_IO_SUBMITTED)) {
        if (io_state == SOLVER_CODEKD_PACKET_QUAD_IO_SUBMITTED) {
            packet->candidate_quad_delivery_disabled = TRUE;
            packet->candidate_star_delivery_disabled = TRUE;
        } else {
            packet->candidate_star_delivery_disabled = TRUE;
        }
        packet->state = SOLVER_CODEKD_PACKET_RESULTS_READY;
        return 3;
    }
    if (io_state == SOLVER_CODEKD_PACKET_RESULTS_READY &&
        packet->verification_delivery_disabled) {
        return 3;
    }
    if (ticket_result < 0 &&
        (io_state == SOLVER_CODEKD_PACKET_VERIFY_IO_SUBMITTED ||
         io_state ==
             SOLVER_CODEKD_PACKET_VERIFY_PAGE_PLAN_COMPLETE)) {
        if (packet->state == SOLVER_CODEKD_PACKET_FAILED) {
            return -1;
        }
        solver_codekd_packet_disable_verification_delivery(packet);
        return 3;
    }
    if (ticket_result > 0 &&
        packet->state == SOLVER_CODEKD_PACKET_RESULTS_READY &&
        !packet->plan_complete) {
        return 3;
    }
    if (ticket_result > 0 && packet->plan_complete &&
        packet->plan_range_count &&
        packet->state == SOLVER_CODEKD_PACKET_PAGE_PLAN_COMPLETE) {
        packet->state = SOLVER_CODEKD_PACKET_COMPUTE_READY;
        return 1;
    }
    if (ticket_result < 0 && packet->plan_complete &&
        packet->state == SOLVER_CODEKD_PACKET_PAGE_PLAN_COMPLETE) {
        if (solver_codekd_packet_mark_plan_owner_replay(packet)) {
            packet->state = SOLVER_CODEKD_PACKET_FAILED;
            return -1;
        }
        return packet->state == SOLVER_CODEKD_PACKET_RESULTS_READY
            ? 3
            : 2;
    }
    packet->state = SOLVER_CODEKD_PACKET_FAILED;
    return -1;
}

int solver_codekd_packet_cancel_pages(
    solver_codekd_search_packet_t* packet) {
    if (!packet || !packet->delivery_ticket) {
        return 0;
    }
    return fitsbin_payload_io_ticket_cancel_async(
        packet->delivery_ticket);
}

index_shard_helper_task_status_t
solver_codekd_packet_decode_quad_ready(
    solver_codekd_search_packet_t* packet,
    anbool* more_work) {
    size_t stars_per_candidate;
    size_t candidate_index;
    size_t descriptor_index;

    if (!packet || !packet->phase || !more_work ||
        !packet->phase->quads || !packet->phase->starkd ||
        !packet->candidate_records ||
        packet->state != SOLVER_CODEKD_PACKET_QUAD_COMPUTE_READY ||
        !packet->candidate_count ||
        !packet->candidate_capacity ||
        !packet->candidate_window_count ||
        packet->candidate_window_count >
            SOLVER_CANDIDATE_DELIVERY_LIMIT ||
        packet->candidate_window_count >
            packet->candidate_capacity ||
        packet->candidate_window_first >
            packet->candidate_count ||
        packet->candidate_window_count >
            packet->candidate_count -
                packet->candidate_window_first ||
        packet->candidate_window_first != packet->candidate_cursor ||
        packet->candidate_window_offset ||
        packet->candidate_quad_ready_count ||
        packet->candidate_star_ready_count ||
        packet->phase->use_radec ||
        packet->phase->descriptor.dimquads <= 0 || packet->phase->descriptor.dimquads > DQMAX) {
        return INDEX_SHARD_HELPER_TASK_ERROR;
    }
    *more_work = FALSE;
    if (index_shard_worker_stop_requested()) {
        packet->state = SOLVER_CODEKD_PACKET_STOPPED;
        return INDEX_SHARD_HELPER_TASK_STOPPED;
    }

    /*
     * Phase admission already identified a dense result stream. Populate
     * every quad-star row in one bounded ticket, but preserve the owner's
     * native A/B scale gate and logical consumption order.
     */
    stars_per_candidate = (size_t)packet->phase->descriptor.dimquads;
    if (packet->candidate_window_count >
        SOLVER_CANDIDATE_STAR_LIMIT / stars_per_candidate) {
        packet->candidate_quad_delivery_disabled = TRUE;
        packet->candidate_star_delivery_disabled = TRUE;
        packet->state = SOLVER_CODEKD_PACKET_RESULTS_READY;
        return INDEX_SHARD_HELPER_TASK_OK;
    }
    packet->candidate_star_count =
        packet->candidate_window_count * stars_per_candidate;
    descriptor_index = packet->retire_descriptor;
    for (candidate_index = 0U;
         candidate_index < packet->candidate_window_count;
         candidate_index++) {
        solver_candidate_delivery_record_t* record =
            &packet->candidate_records[candidate_index];
        size_t global_candidate =
            packet->candidate_window_first + candidate_index;
        size_t star_index;

        while (descriptor_index < packet->count) {
            const solver_codekd_result_slot_t* slot =
                &packet->slots[descriptor_index];
            size_t slot_end;

            if (slot->state != SOLVER_CODEKD_RESULT_READY ||
                !slot->hit_count) {
                descriptor_index++;
                continue;
            }
            if (slot->hit_first > packet->hit_count ||
                (size_t)slot->hit_count >
                    packet->hit_count - slot->hit_first) {
                return INDEX_SHARD_HELPER_TASK_ERROR;
            }
            slot_end = slot->hit_first + (size_t)slot->hit_count;
            if (global_candidate < slot->hit_first) {
                return INDEX_SHARD_HELPER_TASK_ERROR;
            }
            if (global_candidate < slot_end) {
                break;
            }
            descriptor_index++;
        }
        if (descriptor_index >= packet->count) {
            return INDEX_SHARD_HELPER_TASK_ERROR;
        }

        memset(record, 0, sizeof(*record));
        record->descriptor_index = descriptor_index;
        record->quadid = packet->inds[global_candidate];
        if (quadfile_get_stars(
                packet->phase->quads,
                record->quadid,
                record->stars)) {
            packet->candidate_quad_delivery_disabled = TRUE;
            packet->candidate_star_delivery_disabled = TRUE;
            packet->candidate_star_count = 0U;
            packet->state = SOLVER_CODEKD_PACKET_RESULTS_READY;
            return INDEX_SHARD_HELPER_TASK_OK;
        }
        for (star_index = 0U;
             star_index < stars_per_candidate;
             star_index++) {
            packet->candidate_starids[
                candidate_index * stars_per_candidate + star_index] =
                record->stars[star_index];
        }
    }
    packet->candidate_quad_ready_count =
        packet->candidate_window_count;
    if (packet->candidate_star_delivery_disabled) {
        packet->state = SOLVER_CODEKD_PACKET_RESULTS_READY;
        return INDEX_SHARD_HELPER_TASK_OK;
    }
    packet->state = SOLVER_CODEKD_PACKET_STAR_SUBMIT_READY;
    *more_work = TRUE;
    return INDEX_SHARD_HELPER_TASK_OK;
}

index_shard_helper_task_status_t
solver_codekd_packet_copy_star_ready(
    const solver_codekd_packet_task_input_t* input,
    solver_codekd_search_packet_t* packet,
    anbool* more_work) {
    size_t stars_per_candidate;
    size_t candidate_index;

    if (!input || !input->phase ||
        !packet || !packet->phase ||
        packet->phase != input->phase ||
        !more_work || !packet->phase->starkd ||
        packet->state != SOLVER_CODEKD_PACKET_STAR_COMPUTE_READY ||
        !packet->candidate_records || !packet->candidate_count ||
        !packet->candidate_capacity ||
        !packet->candidate_window_count ||
        packet->candidate_window_count >
            SOLVER_CANDIDATE_DELIVERY_LIMIT ||
        packet->candidate_window_count >
            packet->candidate_capacity ||
        packet->candidate_window_first >
            packet->candidate_count ||
        packet->candidate_window_count >
            packet->candidate_count -
                packet->candidate_window_first ||
        packet->candidate_window_first != packet->candidate_cursor ||
        packet->candidate_window_offset ||
        packet->candidate_quad_ready_count !=
            packet->candidate_window_count ||
        packet->candidate_star_ready_count ||
        !packet->candidate_star_count ||
        packet->candidate_star_count > SOLVER_CANDIDATE_STAR_LIMIT ||
        packet->phase->descriptor.dimquads <= 0 || packet->phase->descriptor.dimquads > DQMAX) {
        return INDEX_SHARD_HELPER_TASK_ERROR;
    }
    *more_work = FALSE;
    if (index_shard_worker_stop_requested()) {
        packet->state = SOLVER_CODEKD_PACKET_STOPPED;
        return INDEX_SHARD_HELPER_TASK_STOPPED;
    }

    stars_per_candidate = (size_t)packet->phase->descriptor.dimquads;
    if (packet->candidate_star_count !=
        packet->candidate_window_count * stars_per_candidate) {
        return INDEX_SHARD_HELPER_TASK_ERROR;
    }
    for (candidate_index = 0U;
         candidate_index < packet->candidate_window_count;
         candidate_index++) {
        solver_candidate_delivery_record_t* record =
            &packet->candidate_records[candidate_index];
        size_t star_index;

        for (star_index = 0U;
             star_index < stars_per_candidate;
             star_index++) {
            if (startree_get_ready(
                    packet->phase->starkd,
                    (int)record->stars[star_index],
                    record->starxyz + 3U * star_index)) {
                packet->candidate_star_delivery_disabled = TRUE;
                packet->state = SOLVER_CODEKD_PACKET_RESULTS_READY;
                return INDEX_SHARD_HELPER_TASK_OK;
            }
        }
    }
    packet->candidate_star_ready_count =
        packet->candidate_window_count;
    if (solver_codekd_packet_build_verify_queries(
            packet, input)) {
        for (candidate_index = 0U;
             candidate_index < packet->candidate_window_count;
             candidate_index++) {
            packet->candidate_records[
                candidate_index].candidate_prepared = FALSE;
        }
        packet->verification_delivery_disabled = TRUE;
        packet->state = SOLVER_CODEKD_PACKET_RESULTS_READY;
        return INDEX_SHARD_HELPER_TASK_OK;
    }
    packet->state = packet->candidate_verify_query_count
        ? SOLVER_CODEKD_PACKET_VERIFY_SUBMIT_READY
        : SOLVER_CODEKD_PACKET_RESULTS_READY;
    if (packet->state == SOLVER_CODEKD_PACKET_VERIFY_SUBMIT_READY) {
        *more_work = TRUE;
    }
    return INDEX_SHARD_HELPER_TASK_OK;
}
