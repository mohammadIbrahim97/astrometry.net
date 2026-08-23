/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */
/* CodeKD page-set planning and packet lifecycle. */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "solver.h"
#include "verify.h"
#include "fitsbin.h"
#include "log.h"
#include "index_shard_internal.h"
#include "solver_codekd_internal.h"
#include "solver_inline_internal.h"
#include "solver_hypothesis_internal.h"

solver_codekd_packet_wave_t*
solver_codekd_packet_wave_create(size_t capacity) {
    solver_codekd_packet_wave_t* wave;

    if (!capacity ||
        capacity > SIZE_MAX / sizeof(*wave->inputs) ||
        capacity > SIZE_MAX / sizeof(*wave->packets) ||
        capacity > SIZE_MAX / sizeof(*wave->tasks)) {
        return NULL;
    }
    wave = calloc(1, sizeof(*wave));
    if (!wave) {
        return NULL;
    }
    wave->inputs = calloc(capacity, sizeof(*wave->inputs));
    wave->packets = calloc(capacity, sizeof(*wave->packets));
    wave->tasks = calloc(capacity, sizeof(*wave->tasks));
    if (!wave->inputs || !wave->packets || !wave->tasks) {
        free(wave->tasks);
        free(wave->packets);
        free(wave->inputs);
        free(wave);
        return NULL;
    }
    wave->capacity = capacity;
    return wave;
}

void solver_codekd_packet_wave_destroy(
    solver_codekd_packet_wave_t* wave) {
    if (!wave) {
        return;
    }
    free(wave->tasks);
    free(wave->packets);
    free(wave->inputs);
    free(wave);
}

static uint64_t solver_codekd_page_hash(
    uintptr_t mapping_begin,
    uintptr_t page_key) {
    uint64_t value = (uint64_t)mapping_begin;

    value ^= (uint64_t)page_key + UINT64_C(0x9e3779b97f4a7c15) +
        (value << 6) + (value >> 2);
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return value;
}

static void solver_codekd_page_set_cleanup(
    solver_codekd_page_set_t* set) {
    if (!set) {
        return;
    }
    free(set->entries);
    free(set->hash);
    memset(set, 0, sizeof(*set));
}

static int solver_codekd_page_set_init(
    solver_codekd_page_set_t* set,
    size_t capacity,
    size_t hash_capacity) {
    if (!set || !capacity || hash_capacity < capacity ||
        (hash_capacity & (hash_capacity - 1U)) != 0U ||
        capacity > SIZE_MAX / sizeof(*set->entries) ||
        hash_capacity > SIZE_MAX / sizeof(*set->hash)) {
        return -1;
    }
    memset(set, 0, sizeof(*set));
    set->entries = malloc(capacity * sizeof(*set->entries));
    set->hash = calloc(hash_capacity, sizeof(*set->hash));
    if (!set->entries || !set->hash) {
        solver_codekd_page_set_cleanup(set);
        return 1;
    }
    set->capacity = capacity;
    set->hash_capacity = hash_capacity;
    return 0;
}

void solver_codekd_page_set_reset(
    solver_codekd_page_set_t* set) {
    if (!set) {
        return;
    }
    set->count = 0U;
    set->generation++;
    if (!set->generation) {
        memset(set->hash, 0,
               set->hash_capacity * sizeof(*set->hash));
        set->generation = 1U;
    }
}

static int solver_codekd_page_set_find(
    const solver_codekd_page_set_t* set,
    uintptr_t mapping_begin,
    uintptr_t page_key,
    size_t* slot_out) {
    size_t slot;
    size_t probes;

    if (!set || !set->hash_capacity || !set->generation) {
        return 0;
    }
    slot = (size_t)solver_codekd_page_hash(
        mapping_begin, page_key) & (set->hash_capacity - 1U);
    for (probes = 0U; probes < set->hash_capacity; probes++) {
        size_t entry_index;

        if (set->hash[slot].generation != set->generation) {
            if (slot_out) {
                *slot_out = slot;
            }
            return 0;
        }
        entry_index = set->hash[slot].index;
        if (entry_index < set->count &&
            set->entries[entry_index].mapping_begin == mapping_begin &&
            set->entries[entry_index].page_key == page_key) {
            if (slot_out) {
                *slot_out = slot;
            }
            return 1;
        }
        slot = (slot + 1U) & (set->hash_capacity - 1U);
    }
    return -1;
}

static int solver_codekd_page_set_add(
    solver_codekd_page_set_t* set,
    const solver_codekd_page_entry_t* entry) {
    size_t slot = 0U;
    int found;

    if (!set || !entry) {
        return -1;
    }
    found = solver_codekd_page_set_find(
        set, entry->mapping_begin, entry->page_key, &slot);
    if (found) {
        return found > 0 ? 0 : -1;
    }
    if (set->count >= set->capacity) {
        return 1;
    }
    set->entries[set->count] = *entry;
    set->hash[slot].index = set->count;
    set->hash[slot].generation = set->generation;
    set->count++;
    return 0;
}

static void solver_codekd_page_workspace_cleanup(
    solver_codekd_page_workspace_t* workspace) {
    if (!workspace) {
        return;
    }
    solver_codekd_page_set_cleanup(&workspace->descriptor);
    solver_codekd_page_set_cleanup(&workspace->group);
    free(workspace->sort_entries);
    free(workspace->sealed_ranges);
    free(workspace);
}

int solver_codekd_page_workspace_create(
    solver_codekd_page_workspace_t** workspace_out) {
    solver_codekd_page_workspace_t* workspace;
    size_t hash_capacity = 1U;
    size_t page_limit;
    long detected_page_size;
    int status;

    if (!workspace_out) {
        return -1;
    }
    *workspace_out = NULL;
    detected_page_size = sysconf(_SC_PAGESIZE);
    if (detected_page_size <= 0) {
        return 1;
    }
    page_limit = SOLVER_CODEKD_DELIVERY_BUDGET_BYTES /
        (size_t)detected_page_size;
    if (!page_limit || page_limit > SIZE_MAX / 2U ||
        page_limit > SIZE_MAX /
            sizeof(solver_codekd_page_entry_t) / 2U) {
        return 1;
    }
    while (hash_capacity < page_limit * 2U) {
        if (hash_capacity > SIZE_MAX / 2U) {
            return 1;
        }
        hash_capacity *= 2U;
    }
    workspace = calloc(1, sizeof(*workspace));
    if (!workspace) {
        return 1;
    }
    status = solver_codekd_page_set_init(
        &workspace->descriptor, page_limit, hash_capacity);
    if (!status) {
        status = solver_codekd_page_set_init(
            &workspace->group, page_limit, hash_capacity);
    }
    if (!status) {
        workspace->sort_entries = malloc(
            2U * page_limit * sizeof(*workspace->sort_entries));
        if (workspace->sort_entries) {
            workspace->sort_scratch =
                workspace->sort_entries + page_limit;
        }
        workspace->sealed_ranges = malloc(
            SOLVER_CODEKD_DELIVERY_RANGE_CAPACITY *
                sizeof(*workspace->sealed_ranges));
        if (!workspace->sort_entries || !workspace->sealed_ranges) {
            status = 1;
        }
    }
    if (status) {
        solver_codekd_page_workspace_cleanup(workspace);
        return status;
    }
    workspace->page_size = (size_t)detected_page_size;
    workspace->page_limit = page_limit;
    workspace->sealed_range_capacity =
        SOLVER_CODEKD_DELIVERY_RANGE_CAPACITY;
    solver_codekd_page_set_reset(&workspace->descriptor);
    solver_codekd_page_set_reset(&workspace->group);
    *workspace_out = workspace;
    return 0;
}

static int solver_codekd_page_plan_enabled(
    void* opaque,
    void* mapping) {
    solver_codekd_page_plan_t* plan = opaque;
    fitsbin_t* source = mapping;

    if (!plan || !plan->enabled || !source ||
        plan->source != source) {
        return FALSE;
    }
    if (plan->cancelled &&
        plan->cancelled(plan->cancel_opaque)) {
        plan->cancellation_observed = TRUE;
        return FALSE;
    }
    return TRUE;
}

int solver_codekd_page_plan_emit(
    void* opaque,
    const kdtree_prefetch_hint_t* hint) {
    solver_codekd_page_plan_t* plan = opaque;
    solver_codekd_page_workspace_t* workspace;
    const void* map_base;
    const void* range_start;
    size_t map_size;
    size_t range_size;
    uintptr_t map_begin;
    uintptr_t map_end;
    uintptr_t request_begin;
    uintptr_t request_end;
    uintptr_t page_key;
    int resolved;

    if (!plan || !hint || !hint->mapping ||
        !hint->address || !hint->length) {
        return KDTREE_PREFETCH_EMIT_ERROR;
    }
    if (plan->cancelled &&
        plan->cancelled(plan->cancel_opaque)) {
        plan->cancellation_observed = TRUE;
        return KDTREE_PREFETCH_EMIT_ERROR;
    }
    if (plan->source != hint->mapping || !plan->workspace) {
        return KDTREE_PREFETCH_EMIT_ERROR;
    }
    workspace = plan->workspace;
    resolved = fitsbin_resolve_mapped_range(
        plan->source,
        hint->address,
        hint->length,
        &map_base,
        &map_size,
        &range_start,
        &range_size);
    if (resolved <= 0 || range_size != hint->length) {
        return KDTREE_PREFETCH_EMIT_ERROR;
    }
    map_begin = (uintptr_t)map_base;
    request_begin = (uintptr_t)range_start;
    if (map_size > UINTPTR_MAX - map_begin ||
        range_size > UINTPTR_MAX - request_begin) {
        return KDTREE_PREFETCH_EMIT_ERROR;
    }
    map_end = map_begin + map_size;
    request_end = request_begin + range_size;
    if (request_begin < map_begin || request_end > map_end) {
        return KDTREE_PREFETCH_EMIT_ERROR;
    }
    page_key = request_begin -
        request_begin % (uintptr_t)workspace->page_size;
    while (page_key < request_end) {
        solver_codekd_page_entry_t entry;
        uintptr_t next_page;
        int add_status;

        if ((uintptr_t)workspace->page_size > UINTPTR_MAX - page_key) {
            return KDTREE_PREFETCH_EMIT_ERROR;
        }
        next_page = page_key + (uintptr_t)workspace->page_size;
        entry.mapping_begin = map_begin;
        entry.mapping_end = map_end;
        entry.page_key = page_key;
        entry.populate_begin = MAX(page_key, map_begin);
        entry.populate_end = MIN(next_page, map_end);
        if (entry.populate_end <= entry.populate_begin) {
            return KDTREE_PREFETCH_EMIT_ERROR;
        }
        add_status = solver_codekd_page_set_add(
            &workspace->descriptor, &entry);
        if (add_status > 0) {
            return KDTREE_PREFETCH_EMIT_REFUSED;
        }
        if (add_status < 0) {
            return KDTREE_PREFETCH_EMIT_ERROR;
        }
        page_key = next_page;
    }
    return KDTREE_PREFETCH_EMIT_CONTINUE;
}

static inline int solver_codekd_page_entry_compare(
    const solver_codekd_page_entry_t* left,
    const solver_codekd_page_entry_t* right) {

    if (left->mapping_begin < right->mapping_begin) {
        return -1;
    }
    if (left->mapping_begin > right->mapping_begin) {
        return 1;
    }
    if (left->mapping_end < right->mapping_end) {
        return -1;
    }
    if (left->mapping_end > right->mapping_end) {
        return 1;
    }
    if (left->populate_begin < right->populate_begin) {
        return -1;
    }
    if (left->populate_begin > right->populate_begin) {
        return 1;
    }
    return 0;
}

/*
 * Page plans contain at most one delivery budget of entries. Keep the sort
 * workspace owner-local and compare entries directly so sealing does not pay
 * a general-purpose qsort callback cost per packet.
 */
static void solver_codekd_page_entries_sort(
    solver_codekd_page_workspace_t* workspace,
    size_t count) {
    solver_codekd_page_entry_t* source;
    solver_codekd_page_entry_t* destination;
    size_t width;

    if (!workspace || count < 2U) {
        return;
    }
    source = workspace->sort_entries;
    destination = workspace->sort_scratch;
    width = 1U;
    while (width < count) {
        size_t left = 0U;

        while (left < count) {
            size_t middle = left + MIN(width, count - left);
            size_t right = middle + MIN(width, count - middle);
            size_t left_cursor = left;
            size_t right_cursor = middle;
            size_t output = left;

            while (left_cursor < middle && right_cursor < right) {
                if (solver_codekd_page_entry_compare(
                        &source[left_cursor],
                        &source[right_cursor]) <= 0) {
                    destination[output++] = source[left_cursor++];
                } else {
                    destination[output++] = source[right_cursor++];
                }
            }
            while (left_cursor < middle) {
                destination[output++] = source[left_cursor++];
            }
            while (right_cursor < right) {
                destination[output++] = source[right_cursor++];
            }
            left = right;
        }
        {
            solver_codekd_page_entry_t* swap = source;

            source = destination;
            destination = swap;
        }
        if (width > count / 2U) {
            width = count;
        } else {
            width *= 2U;
        }
    }
    if (source != workspace->sort_entries) {
        memcpy(workspace->sort_entries,
               source,
               count * sizeof(*source));
    }
}

/*
 * Materialize the physical union only after page-key deduplication. Sorting
 * this private page list cannot change descriptor or KD-hit order.
 */
int solver_codekd_page_plan_seal_union(
    solver_codekd_page_workspace_t* workspace,
    anbool include_descriptor,
    size_t* unique_pages_out,
    size_t* range_count_out,
    size_t* aligned_bytes_out) {
    size_t unique_pages = 0U;
    size_t range_count = 0U;
    size_t aligned_bytes = 0U;
    size_t i;

    if (!workspace || !unique_pages_out || !range_count_out ||
        !aligned_bytes_out) {
        return -1;
    }
    if (workspace->group.count > workspace->page_limit) {
        return -1;
    }
    for (i = 0U; i < workspace->group.count; i++) {
        workspace->sort_entries[unique_pages++] =
            workspace->group.entries[i];
    }
    if (include_descriptor) {
        for (i = 0U; i < workspace->descriptor.count; i++) {
            const solver_codekd_page_entry_t* entry =
                &workspace->descriptor.entries[i];
            int found = solver_codekd_page_set_find(
                &workspace->group,
                entry->mapping_begin,
                entry->page_key,
                NULL);

            if (found < 0) {
                return -1;
            }
            if (found) {
                continue;
            }
            if (unique_pages >= workspace->page_limit) {
                return 1;
            }
            workspace->sort_entries[unique_pages++] = *entry;
        }
    }
    if (!unique_pages) {
        *unique_pages_out = 0U;
        *range_count_out = 0U;
        *aligned_bytes_out = 0U;
        return 0;
    }
    solver_codekd_page_entries_sort(workspace, unique_pages);
    for (i = 0U; i < unique_pages; i++) {
        const solver_codekd_page_entry_t* entry =
            &workspace->sort_entries[i];
        fitsbin_prefetch_range_t* previous = range_count
            ? &workspace->sealed_ranges[range_count - 1U]
            : NULL;
        uintptr_t previous_begin = previous
            ? (uintptr_t)previous->data
            : 0U;
        uintptr_t previous_end = previous_begin;

        assert(!i || solver_codekd_page_entry_compare(
            &workspace->sort_entries[i - 1U], entry) <= 0);
        if (previous && previous->size <=
                UINTPTR_MAX - previous_begin) {
            previous_end += previous->size;
        }
        if (previous &&
            entry->mapping_begin ==
                workspace->sort_entries[i - 1U].mapping_begin &&
            entry->mapping_end ==
                workspace->sort_entries[i - 1U].mapping_end &&
            entry->populate_begin <= previous_end) {
            if (entry->populate_end > previous_end) {
                previous->size = (size_t)(
                    entry->populate_end - previous_begin);
            }
            continue;
        }
        if (range_count >= workspace->sealed_range_capacity) {
            return 2;
        }
        workspace->sealed_ranges[range_count].data =
            (const void*)entry->populate_begin;
        workspace->sealed_ranges[range_count].size =
            (size_t)(entry->populate_end - entry->populate_begin);
        range_count++;
    }
    for (i = 0U; i < range_count; i++) {
        if (workspace->sealed_ranges[i].size >
            SIZE_MAX - aligned_bytes) {
            return -1;
        }
        aligned_bytes += workspace->sealed_ranges[i].size;
    }
    if (aligned_bytes > SOLVER_CODEKD_DELIVERY_BUDGET_BYTES) {
        return 1;
    }
    *unique_pages_out = unique_pages;
    *range_count_out = range_count;
    *aligned_bytes_out = aligned_bytes;
    return 0;
}

int solver_codekd_page_set_merge_descriptor(
    solver_codekd_page_workspace_t* workspace) {
    size_t i;

    if (!workspace) {
        return -1;
    }
    for (i = 0U; i < workspace->descriptor.count; i++) {
        int add_status = solver_codekd_page_set_add(
            &workspace->group,
            &workspace->descriptor.entries[i]);

        if (add_status) {
            return -1;
        }
    }
    return 0;
}

/*
 * Check whether the current descriptor page set fits in the group without
 * sorting or materializing physical ranges. The group hash already defines
 * the exact mapped-page identity used by the final seal.
 */
int solver_codekd_page_set_union_fits(
    const solver_codekd_page_workspace_t* workspace) {
    size_t additional = 0U;
    size_t i;

    if (!workspace ||
        workspace->group.count > workspace->page_limit) {
        return -1;
    }
    for (i = 0U; i < workspace->descriptor.count; i++) {
        const solver_codekd_page_entry_t* entry =
            &workspace->descriptor.entries[i];
        int found = solver_codekd_page_set_find(
            &workspace->group,
            entry->mapping_begin,
            entry->page_key,
            NULL);

        if (found < 0) {
            return -1;
        }
        if (found) {
            continue;
        }
        if (additional >=
            workspace->page_limit - workspace->group.count) {
            return 0;
        }
        additional++;
    }
    return 1;
}

anbool solver_codekd_packet_plan_cancelled(void* opaque) {
    (void)opaque;
    return index_shard_worker_stop_requested();
}

void solver_codekd_packet_reset_page_plan(
    solver_codekd_search_packet_t* packet) {
    packet->plan_first = 0U;
    packet->plan_end = 0U;
    packet->plan_range_count = 0U;
    packet->plan_complete = FALSE;
}

/*
 * Plan the next contiguous descriptor slice transactionally. A positive
 * result exposes one complete sealed plan. Zero means every descriptor now
 * has either a populated-query result pending or exact owner replay. No
 * incomplete prefix is ever submitted.
 */
int solver_codekd_search_packet_prepare_next_plan(
    solver_codekd_search_packet_t* packet,
    fitsbin_payload_io_cancel_check_fn cancelled,
    void* cancel_opaque) {
    solver_codekd_page_workspace_t* workspace;
    fitsbin_t* source;
    size_t group_first = 0U;
    size_t group_end = 0U;

    if (!packet || !packet->phase || !packet->phase->tree ||
        !packet->phase->tree->io || !packet->phase->tree->io_is_fitsbin ||
        !packet->descriptors || !packet->slots ||
        !packet->page_workspace || !cancelled ||
        packet->state != SOLVER_CODEKD_PACKET_DESCRIPTORS_READY ||
        packet->next_descriptor > packet->count) {
        return -1;
    }
    workspace = packet->page_workspace;
    source = (fitsbin_t*)packet->phase->tree->io;
    solver_codekd_packet_reset_page_plan(packet);
    solver_codekd_page_set_reset(&workspace->group);

    while (packet->next_descriptor < packet->count) {
        const solver_ab_descriptor_t* descriptor =
            &packet->descriptors->descriptors[
                packet->next_descriptor];
        solver_codekd_page_plan_t plan;
        kdtree_prefetch_sink_t sink;
        size_t unique_pages;
        size_t range_count;
        size_t aligned_bytes;
        int prepare_status;
        int seal_status;

        if (cancelled(cancel_opaque)) {
            packet->state = SOLVER_CODEKD_PACKET_STOPPED;
            return 2;
        }
        if (!packet->pending_descriptor_plan) {
            solver_codekd_page_set_reset(&workspace->descriptor);
            memset(&plan, 0, sizeof(plan));
            memset(&sink, 0, sizeof(sink));
            plan.source = source;
            plan.workspace = workspace;
            plan.cancelled = cancelled;
            plan.cancel_opaque = cancel_opaque;
            plan.enabled = TRUE;
            sink.userdata = &plan;
            sink.enabled = solver_codekd_page_plan_enabled;
            sink.emit = solver_codekd_page_plan_emit;
            prepare_status = kdtree_rangesearch_prefetch_prepare(
                packet->phase->tree,
                descriptor->code,
                descriptor->tol2,
                SOLVER_CODEKD_SEARCH_OPTIONS,
                &sink);
            if (plan.cancellation_observed) {
                packet->state = SOLVER_CODEKD_PACKET_STOPPED;
                return 2;
            }
            if (prepare_status !=
                    KDTREE_PREFETCH_PREPARE_COMPLETE ||
                !workspace->descriptor.count) {
                packet->slots[packet->next_descriptor].state =
                    SOLVER_CODEKD_RESULT_OWNER_REPLAY;
                packet->next_descriptor++;
                if (workspace->group.count) {
                    break;
                }
                continue;
            }
        }

        seal_status = solver_codekd_page_plan_seal_union(
            workspace,
            TRUE,
            &unique_pages,
            &range_count,
            &aligned_bytes);
        if (seal_status) {
            if (workspace->group.count) {
                packet->pending_descriptor_plan = TRUE;
                break;
            }
            packet->slots[packet->next_descriptor].state =
                SOLVER_CODEKD_RESULT_OWNER_REPLAY;
            packet->pending_descriptor_plan = FALSE;
            packet->next_descriptor++;
            continue;
        }
        if (!workspace->group.count) {
            group_first = packet->next_descriptor;
        }
        if (solver_codekd_page_set_merge_descriptor(workspace)) {
            return -1;
        }
        packet->pending_descriptor_plan = FALSE;
        packet->next_descriptor++;
        group_end = packet->next_descriptor;
        if (workspace->group.count >= workspace->page_limit) {
            break;
        }
    }

    if (!workspace->group.count) {
        if (packet->next_descriptor != packet->count) {
            return -1;
        }
        packet->state = SOLVER_CODEKD_PACKET_RESULTS_READY;
        return 0;
    }
    {
        size_t unique_pages;
        size_t range_count;
        size_t aligned_bytes;
        int seal_status = solver_codekd_page_plan_seal_union(
            workspace,
            FALSE,
            &unique_pages,
            &range_count,
            &aligned_bytes);

        if (seal_status || !unique_pages || !range_count) {
            return -1;
        }
        packet->plan_first = group_first;
        packet->plan_end = group_end;
        packet->plan_range_count = range_count;
        packet->plan_complete = TRUE;
        packet->state = SOLVER_CODEKD_PACKET_PAGE_PLAN_COMPLETE;
    }
    return 1;
}

void solver_codekd_packet_reset_verify_plan(
    solver_codekd_search_packet_t* packet) {
    size_t candidate_index;
    size_t retained_bytes = 0U;

    if (!packet) {
        return;
    }
    packet->verify_plan_first = 0U;
    packet->verify_plan_end = 0U;
    packet->verify_topology_end = 0U;
    packet->verify_plan_range_count = 0U;
    packet->verify_plan_complete = FALSE;
    packet->verify_sweep_range_count = 0U;
    packet->verify_sweep_aligned_bytes = 0U;
    packet->verify_sweep_plan_complete = FALSE;
    for (candidate_index = 0U;
         packet->candidate_records &&
         candidate_index < packet->candidate_window_count;
         candidate_index++) {
        size_t query_bytes = verify_index_query_bytes(
            packet->candidate_records[
                candidate_index].verify_query);

        if (query_bytes == SIZE_MAX ||
            query_bytes > SIZE_MAX - retained_bytes) {
            retained_bytes = SIZE_MAX;
            break;
        }
        retained_bytes += query_bytes;
    }
    packet->verify_query_bytes = retained_bytes;
}

void solver_codekd_packet_disable_verification_delivery(
    solver_codekd_search_packet_t* packet) {
    if (!packet) {
        return;
    }
    solver_codekd_packet_reset_verify_plan(packet);
    packet->verify_pending_query_index = 0U;
    packet->verify_pending_query_plan = FALSE;
    packet->verification_delivery_disabled = TRUE;
    packet->state = SOLVER_CODEKD_PACKET_RESULTS_READY;
}

/*
 * Build the maximal complete canonical verification-query prefix. Each query
 * is transactional: no page from an incomplete query enters a READY prefix.
 */
static int solver_codekd_search_packet_prepare_verify_plan(
    solver_codekd_search_packet_t* packet,
    fitsbin_payload_io_cancel_check_fn cancelled,
    void* cancel_opaque) {
    solver_codekd_page_workspace_t* workspace;
    fitsbin_t* source;
    size_t segment_first;
    size_t segment_end;
    size_t candidate_index;

    if (!packet || !packet->phase ||
        !packet->phase->starkd || !packet->phase->starkd->tree ||
        !packet->phase->starkd->tree->io ||
        !packet->phase->starkd->tree->io_is_fitsbin ||
        !packet->candidate_records || !packet->page_workspace ||
        !cancelled ||
        packet->state != SOLVER_CODEKD_PACKET_VERIFY_IO_SUBMITTED ||
        packet->candidate_window_offset >=
            packet->candidate_window_count ||
        packet->candidate_star_ready_count !=
            packet->candidate_window_count ||
        packet->verification_delivery_disabled) {
        return -1;
    }
    workspace = packet->page_workspace;
    source = (fitsbin_t*)packet->phase->starkd->tree->io;
    segment_first = packet->candidate_window_offset;
    segment_end = segment_first;
    solver_codekd_packet_reset_verify_plan(packet);
    solver_codekd_page_set_reset(&workspace->group);

    for (candidate_index = segment_first;
         candidate_index < packet->candidate_window_count;
         candidate_index++) {
        solver_candidate_delivery_record_t* record =
            &packet->candidate_records[candidate_index];
        solver_codekd_page_plan_t plan;
        kdtree_prefetch_sink_t sink;
        size_t unique_pages;
        size_t range_count;
        size_t aligned_bytes;
        int prepare_status;
        int seal_status;

        if (cancelled(cancel_opaque)) {
            packet->state = SOLVER_CODEKD_PACKET_STOPPED;
            return 2;
        }
        if (record->verify_delivery_fallback) {
            if (workspace->group.count) {
                break;
            }
            solver_codekd_packet_disable_verification_delivery(packet);
            return 0;
        }
        if (record->plan_action !=
            SOLVER_AB_CANDIDATE_VERIFY) {
            segment_end = candidate_index + 1U;
            continue;
        }

        if (packet->verify_pending_query_plan) {
            if (packet->verify_pending_query_index != candidate_index ||
                !workspace->descriptor.count) {
                return -1;
            }
        } else {
            solver_codekd_page_set_reset(&workspace->descriptor);
            memset(&plan, 0, sizeof(plan));
            memset(&sink, 0, sizeof(sink));
            plan.source = source;
            plan.workspace = workspace;
            plan.cancelled = cancelled;
            plan.cancel_opaque = cancel_opaque;
            plan.enabled = TRUE;
            sink.userdata = &plan;
            sink.enabled = solver_codekd_page_plan_enabled;
            sink.emit = solver_codekd_page_plan_emit;
            prepare_status = kdtree_rangesearch_prefetch_prepare(
                packet->phase->starkd->tree,
                record->verify_center,
                record->verify_radius2,
                SOLVER_STARKD_VERIFY_SEARCH_OPTIONS,
                &sink);
            if (plan.cancellation_observed) {
                packet->state = SOLVER_CODEKD_PACKET_STOPPED;
                return 2;
            }
            if (prepare_status !=
                KDTREE_PREFETCH_PREPARE_COMPLETE) {
                record->verify_delivery_fallback = TRUE;
                if (workspace->group.count) {
                    break;
                }
                solver_codekd_packet_disable_verification_delivery(
                    packet);
                return 0;
            }
            if (!workspace->descriptor.count) {
                segment_end = candidate_index + 1U;
                continue;
            }
        }

        seal_status = solver_codekd_page_plan_seal_union(
            workspace,
            TRUE,
            &unique_pages,
            &range_count,
            &aligned_bytes);
        if (seal_status) {
            if (workspace->group.count) {
                packet->verify_pending_query_plan = TRUE;
                packet->verify_pending_query_index = candidate_index;
                break;
            }
            record->verify_delivery_fallback = TRUE;
            solver_codekd_packet_disable_verification_delivery(packet);
            return 0;
        }
        if (solver_codekd_page_set_merge_descriptor(workspace)) {
            return -1;
        }
        packet->verify_pending_query_plan = FALSE;
        packet->verify_pending_query_index = 0U;
        segment_end = candidate_index + 1U;
        if (workspace->group.count >= workspace->page_limit) {
            break;
        }
    }

    if (segment_end <= segment_first) {
        return -1;
    }
    packet->verify_plan_first = segment_first;
    packet->verify_plan_end = segment_end;
    packet->verify_topology_end = segment_end;
    packet->verify_plan_complete = TRUE;
    if (!workspace->group.count) {
        packet->state =
            SOLVER_CODEKD_PACKET_VERIFY_PAGE_PLAN_COMPLETE;
        return 0;
    }

    {
        size_t unique_pages;
        size_t range_count;
        size_t aligned_bytes;
        int seal_status = solver_codekd_page_plan_seal_union(
            workspace,
            FALSE,
            &unique_pages,
            &range_count,
            &aligned_bytes);

        if (seal_status || !unique_pages || !range_count) {
            return -1;
        }
        packet->verify_plan_range_count = range_count;
        packet->state =
            SOLVER_CODEKD_PACKET_VERIFY_PAGE_PLAN_COMPLETE;
    }
    return 1;
}

int solver_codekd_packet_plan_verification_pages(
    void* opaque,
    fitsbin_payload_io_cancel_check_fn cancelled,
    void* cancel_opaque,
    fitsbin_prefetch_range_t* ranges,
    size_t range_capacity,
    size_t* range_count) {
    solver_codekd_search_packet_t* packet = opaque;
    int plan_status;

    if (!packet || !cancelled || !ranges || !range_count) {
        errno = EINVAL;
        return -1;
    }
    *range_count = 0U;
    plan_status = solver_codekd_search_packet_prepare_verify_plan(
        packet, cancelled, cancel_opaque);
    if (plan_status < 0) {
        packet->state = SOLVER_CODEKD_PACKET_FAILED;
        errno = EIO;
        return -1;
    }
    if (plan_status == 2) {
        errno = ECANCELED;
        return -1;
    }
    if (!packet->verify_plan_complete ||
        packet->state !=
            SOLVER_CODEKD_PACKET_VERIFY_PAGE_PLAN_COMPLETE ||
        !packet->page_workspace ||
        packet->verify_plan_range_count > range_capacity) {
        packet->state = SOLVER_CODEKD_PACKET_FAILED;
        errno = packet->verify_plan_range_count > range_capacity
            ? E2BIG
            : EINVAL;
        return -1;
    }
    if (!plan_status) {
        return 0;
    }
    if (!packet->verify_plan_range_count) {
        return 1;
    }
    memcpy(
        ranges,
        packet->page_workspace->sealed_ranges,
        packet->verify_plan_range_count * sizeof(*ranges));
    *range_count = packet->verify_plan_range_count;
    return 1;
}

int solver_codekd_search_packet_release_ticket(
    solver_codekd_search_packet_t* packet) {
    int ticket_result = 0;
    int status;

    if (!packet) {
        return -1;
    }
    if (!packet->delivery_ticket) {
        return 0;
    }
    status = fitsbin_payload_io_ticket_drain_and_destroy(
        &packet->delivery_ticket,
        &ticket_result);
    if (status != 1 || packet->delivery_ticket) {
        return -1;
    }
    return 0;
}

void solver_codekd_record_clear_prepared_verification(
    solver_candidate_delivery_record_t* record) {
    if (!record) {
        return;
    }
    verify_destroy_prepared_score(&record->prepared_score);
    verify_destroy_prepared_hit(record->prepared_verification);
    record->prepared_verification = NULL;
    record->verification_score_ready = FALSE;
}

void solver_codekd_record_clear_verification_speculation(
    solver_candidate_delivery_record_t* record) {
    if (!record) {
        return;
    }
    verify_destroy_index_query(record->verify_query);
    record->verify_query = NULL;
    record->verify_query_captured = FALSE;
    solver_codekd_record_clear_prepared_verification(record);
}

int solver_codekd_search_packet_cleanup(
    solver_codekd_search_packet_t* packet) {
    size_t candidate_index;

    if (!packet ||
        solver_codekd_search_packet_release_ticket(packet)) {
        return -1;
    }
    for (candidate_index = 0U;
         packet->candidate_records &&
         candidate_index < packet->candidate_capacity;
         candidate_index++) {
        solver_codekd_record_clear_verification_speculation(
            &packet->candidate_records[candidate_index]);
    }
    solver_codekd_page_workspace_cleanup(packet->page_workspace);
    free(packet->slots);
    free(packet->inds);
    free(packet->sdists);
    free(packet->candidate_records);
    memset(packet, 0, sizeof(*packet));
    return 0;
}
