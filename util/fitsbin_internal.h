/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */
#ifndef FITSBIN_INTERNAL_H
#define FITSBIN_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include "fitsbin.h"

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define ASTROMETRY_THREAD_LOCAL _Thread_local
#elif defined(__GNUC__)
#define ASTROMETRY_THREAD_LOCAL __thread
#else
#error "A thread-local storage implementation is required"
#endif

#define FITSBIN_PREFETCH_RANGE_LIMIT 640U
#define FITSBIN_PAYLOAD_IO_MAX_LANES 4
#define FITSBIN_PAYLOAD_IO_MAX_JOBS 23U
#define FITSBIN_PAYLOAD_IO_MAX_BYTES \
    (48U * 1024U * 1024U)

typedef struct fitsbin_prefetch_range {
    const void* data;
    size_t size;
} fitsbin_prefetch_range_t;

int fitsbin_configure_index_mmap(fitsbin_t* fb);
int fitsbin_close_payload_fd(fitsbin_t* fb);
int fitsbin_resolve_mapped_range(
    fitsbin_t* fb,
    const void* data,
    size_t size,
    const void** map_base,
    size_t* map_size,
    const void** range_start,
    size_t* range_size);
int fitsbin_set_mmap_range_advice(
    fitsbin_t* fb,
    const void* data,
    size_t size,
    fitsbin_mmap_advice_t advice);
int fitsbin_read_chunk_header(
    fitsbin_t* fb,
    fitsbin_chunk_t* chunk);
int fitsbin_get_open_file_stat(
    const fitsbin_t* fb,
    struct stat* file_stat);
const char* fitsbin_mmap_advice_name(
    fitsbin_mmap_advice_t advice);
fitsbin_mmap_advice_t fitsbin_get_mmap_advice(
    const fitsbin_t* fb);
fitsbin_mmap_advice_t fitsbin_get_chunk_mmap_advice(
    const fitsbin_t* fb,
    const fitsbin_chunk_t* chunk);
void fitsbin_mmap_set_thread_advice(
    fitsbin_mmap_advice_t advice);
void fitsbin_mmap_clear_thread_advice(void);
fitsbin_mmap_advice_t fitsbin_mmap_current_advice(void);
int fitsbin_set_mmap_advice(
    fitsbin_t* fb,
    fitsbin_mmap_advice_t advice,
    anbool reapply_existing);

typedef struct fitsbin_payload_io_ticket
    fitsbin_payload_io_ticket_t;

typedef anbool (*fitsbin_payload_io_cancel_check_fn)(void* opaque);
typedef int (*fitsbin_payload_io_plan_fn)(
    void* opaque,
    fitsbin_payload_io_cancel_check_fn cancelled,
    void* cancel_opaque,
    fitsbin_prefetch_range_t* ranges,
    size_t range_capacity,
    size_t* range_count);

typedef enum fitsbin_payload_io_submit_status {
    FITSBIN_PAYLOAD_IO_SUBMIT_ERROR = -1,
    FITSBIN_PAYLOAD_IO_SUBMIT_UNAVAILABLE = 0,
    FITSBIN_PAYLOAD_IO_SUBMIT_QUEUED = 1,
    FITSBIN_PAYLOAD_IO_SUBMIT_READY = 2
} fitsbin_payload_io_submit_status_t;

typedef void (*fitsbin_payload_io_completion_notify_fn)(
    void* opaque,
    unsigned long long completion_id);

void fitsbin_payload_io_configure_workers(int worker_count);
int fitsbin_payload_io_service_start(int lane_count);
void fitsbin_payload_io_service_stop(void);
int fitsbin_payload_io_service_width(void);
int fitsbin_payload_io_mapped_population_supported(void);
int fitsbin_payload_io_set_completion_notifier(
    fitsbin_payload_io_completion_notify_fn notify,
    void* opaque);
int fitsbin_payload_io_clear_completion_notifier(
    fitsbin_payload_io_completion_notify_fn notify,
    void* opaque);

/* Tickets borrow their source and mappings until terminal collection. */
int fitsbin_prefetch_ranges_submit(
    fitsbin_t* fb,
    const fitsbin_prefetch_range_t* ranges,
    size_t range_count,
    size_t byte_budget,
    fitsbin_payload_io_ticket_t** ticket);
int fitsbin_prefetch_ranges_planned_submit(
    fitsbin_t* fb,
    fitsbin_payload_io_plan_fn plan,
    void* plan_opaque,
    size_t byte_budget,
    fitsbin_payload_io_ticket_t** ticket);
int fitsbin_payload_io_ticket_poll_and_destroy(
    fitsbin_payload_io_ticket_t** ticket_io,
    int* result_out);
int fitsbin_payload_io_ticket_drain_and_destroy(
    fitsbin_payload_io_ticket_t** ticket_io,
    int* result_out);
int fitsbin_payload_io_ticket_cancel_async(
    fitsbin_payload_io_ticket_t* ticket);
unsigned long long fitsbin_payload_io_ticket_completion_id(
    const fitsbin_payload_io_ticket_t* ticket);

/*
 * Join only a one-page hole inside one exact mapping, and spend no more than
 * one quarter of the ticket's exact aligned bytes on those holes. This cuts
 * small syscall fragments without recreating broad mmap readahead.
 */
#define FITSBIN_PAYLOAD_MAPPED_COALESCE_GAP_PAGES 1U
#define FITSBIN_PAYLOAD_MAPPED_COALESCE_BUDGET_DIVISOR 4U

/*
 * File-offset readahead may bridge a slightly wider hole than mapped
 * completion. This changes only storage queue shape: the exact mapped spans
 * remain the sole MADV_POPULATE_READ and READY authority.
 */
#define FITSBIN_PAYLOAD_QUEUE_COALESCE_GAP_PAGES 2U
#define FITSBIN_PAYLOAD_QUEUE_COALESCE_BUDGET_DIVISOR 8U

typedef struct fitsbin_file_span {
    off_t begin;
    off_t end;
} fitsbin_file_span_t;

typedef struct fitsbin_mapped_span {
    uintptr_t map_begin;
    uintptr_t map_end;
    uintptr_t begin;
    uintptr_t end;
} fitsbin_mapped_span_t;

int fitsbin_prefetch_row_budget(
    const fitsbin_t* fb,
    size_t row_size,
    size_t row_count,
    size_t* byte_budget);
int fitsbin_payload_fd_get(fitsbin_t* fb);
int fitsbin_prepare_mapped_spans(
    fitsbin_t* fb,
    const fitsbin_prefetch_range_t* ranges,
    size_t range_count,
    size_t byte_budget,
    fitsbin_mapped_span_t* spans,
    size_t span_capacity,
    size_t* span_count,
    size_t* byte_count);
int fitsbin_prepare_mapped_file_spans(
    fitsbin_t* fb,
    const fitsbin_mapped_span_t* mapped,
    size_t mapped_count,
    size_t byte_budget,
    fitsbin_file_span_t* file_spans,
    size_t file_span_capacity,
    size_t* file_span_count,
    size_t* byte_count);

void fitsbin_apply_mmap_advice(
    fitsbin_t* fb,
    fitsbin_chunk_t* chunk);

#endif
