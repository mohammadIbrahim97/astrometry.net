/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <unistd.h>

#include "keywords.h"
#include "fitsbin.h"
#include "fitsbin_internal.h"

typedef enum fitsbin_payload_io_ticket_state {
    FITSBIN_PAYLOAD_IO_SUBMITTED = 0,
    FITSBIN_PAYLOAD_IO_READY,
    FITSBIN_PAYLOAD_IO_FAILED,
    FITSBIN_PAYLOAD_IO_CANCELLED
} fitsbin_payload_io_ticket_state_t;

typedef enum fitsbin_payload_service_state {
    FITSBIN_PAYLOAD_SERVICE_STOPPED = 0,
    FITSBIN_PAYLOAD_SERVICE_RUNNING,
    FITSBIN_PAYLOAD_SERVICE_STOPPING
} fitsbin_payload_service_state_t;

struct fitsbin_payload_io_ticket {
    fitsbin_prefetch_range_t
        planned_ranges[FITSBIN_PREFETCH_RANGE_LIMIT];
    fitsbin_mapped_span_t spans[FITSBIN_PREFETCH_RANGE_LIMIT];
    fitsbin_file_span_t
        queued_spans[FITSBIN_PREFETCH_RANGE_LIMIT];
    fitsbin_t* source;
    size_t span_count;
    size_t queued_span_count;
    size_t admission_byte_count;
    unsigned long long sequence;
    fitsbin_payload_io_ticket_state_t state;
    fitsbin_payload_io_plan_fn plan;
    void* plan_opaque;
    struct fitsbin_payload_io_ticket* next;
    pthread_cond_t completion_cv;
    int fd;
    int saved_errno;
    anbool cancel_requested;
    anbool owner_draining;
};

typedef struct fitsbin_payload_queue {
    fitsbin_payload_io_ticket_t* head;
    fitsbin_payload_io_ticket_t* tail;
} fitsbin_payload_queue_t;

typedef struct fitsbin_payload_completion_notifier {
    fitsbin_payload_io_completion_notify_fn notify;
    void* opaque;
    size_t active;
    anbool clearing;
} fitsbin_payload_completion_notifier_t;

typedef struct fitsbin_payload_service {
    pthread_mutex_t mutex;
    pthread_cond_t work_cv;
    pthread_cond_t completion_cv;

    fitsbin_payload_queue_t queue;
    fitsbin_payload_completion_notifier_t completion;

    pthread_t threads[FITSBIN_PAYLOAD_IO_MAX_LANES];
    size_t jobs;
    size_t bytes;
    unsigned long long next_sequence;
    int limit;
    int active;
    int lanes;
    fitsbin_payload_service_state_t state;
} fitsbin_payload_service_t;

static fitsbin_payload_service_t service = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .work_cv = PTHREAD_COND_INITIALIZER,
    .completion_cv = PTHREAD_COND_INITIALIZER,
    .limit = 1,
};

static ASTROMETRY_THREAD_LOCAL anbool
    fitsbin_payload_io_planning_active = FALSE;
static ASTROMETRY_THREAD_LOCAL unsigned int
    fitsbin_payload_io_completion_dispatch_depth = 0U;

/* service.mutex must be held. */
static fitsbin_payload_io_completion_notify_fn
fitsbin_payload_io_completion_acquire_locked(void** opaque) {
    fitsbin_payload_io_completion_notify_fn notify =
        service.completion.notify;

    if (opaque) {
        *opaque = NULL;
    }
    if (!notify || service.completion.clearing) {
        return NULL;
    }
    service.completion.active++;
    if (opaque) {
        *opaque = service.completion.opaque;
    }
    return notify;
}

static anbool fitsbin_payload_io_ticket_cancelled(void* opaque) {
    const fitsbin_payload_io_ticket_t* ticket = opaque;

    return ticket && __atomic_load_n(
        &ticket->cancel_requested,
        __ATOMIC_ACQUIRE);
}

static void fitsbin_payload_io_completion_release(void) {
    pthread_mutex_lock(&service.mutex);
    assert(service.completion.active > 0U);
    service.completion.active--;
    pthread_cond_broadcast(&service.completion_cv);
    pthread_mutex_unlock(&service.mutex);
}

static void fitsbin_payload_io_completion_dispatch(
    fitsbin_payload_io_completion_notify_fn notify,
    void* opaque,
    unsigned long long completion_id) {
    if (!notify) {
        return;
    }
    fitsbin_payload_io_completion_dispatch_depth++;
    notify(opaque, completion_id);
    fitsbin_payload_io_completion_dispatch_depth--;
    fitsbin_payload_io_completion_release();
}

int fitsbin_payload_io_set_completion_notifier(
    fitsbin_payload_io_completion_notify_fn notify,
    void* opaque) {
    if (!notify) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&service.mutex);
    if (service.completion.notify ||
        service.completion.clearing) {
        pthread_mutex_unlock(&service.mutex);
        errno = EBUSY;
        return -1;
    }
    service.completion.notify = notify;
    service.completion.opaque = opaque;
    pthread_mutex_unlock(&service.mutex);
    return 0;
}

int fitsbin_payload_io_wait_completion_notifier_idle(
    fitsbin_payload_io_completion_notify_fn notify,
    void* opaque) {
    if (!notify) {
        errno = EINVAL;
        return -1;
    }
    if (fitsbin_payload_io_completion_dispatch_depth) {
        errno = EDEADLK;
        return -1;
    }
    pthread_mutex_lock(&service.mutex);
    if (service.completion.notify != notify ||
        service.completion.opaque != opaque) {
        pthread_mutex_unlock(&service.mutex);
        errno = ENOENT;
        return -1;
    }
    while (service.completion.active) {
        pthread_cond_wait(
            &service.completion_cv,
            &service.mutex);
    }
    pthread_mutex_unlock(&service.mutex);
    return 0;
}

int fitsbin_payload_io_clear_completion_notifier(
    fitsbin_payload_io_completion_notify_fn notify,
    void* opaque) {
    if (!notify) {
        errno = EINVAL;
        return -1;
    }
    if (fitsbin_payload_io_completion_dispatch_depth) {
        errno = EDEADLK;
        return -1;
    }
    pthread_mutex_lock(&service.mutex);
    if (service.completion.notify != notify ||
        service.completion.opaque != opaque) {
        pthread_mutex_unlock(&service.mutex);
        errno = ENOENT;
        return -1;
    }
    if (service.completion.clearing) {
        pthread_mutex_unlock(&service.mutex);
        errno = EBUSY;
        return -1;
    }
    service.completion.clearing = TRUE;
    while (service.completion.active) {
        pthread_cond_wait(
            &service.completion_cv,
            &service.mutex);
    }
    service.completion.notify = NULL;
    service.completion.opaque = NULL;
    service.completion.clearing = FALSE;
    pthread_mutex_unlock(&service.mutex);
    return 0;
}

void fitsbin_payload_io_configure_workers(int worker_count) {
    int capacity = MAX(worker_count, 1);

    pthread_mutex_lock(&service.mutex);
    service.limit = capacity;
    pthread_cond_broadcast(&service.work_cv);
    pthread_mutex_unlock(&service.mutex);
}

static int fitsbin_payload_io_duplicate_fd(int fd) {
    int duplicate;

#if defined(F_DUPFD_CLOEXEC)
    duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 0);
#else
    duplicate = dup(fd);
    if (duplicate >= 0) {
        (void)fcntl(duplicate, F_SETFD, FD_CLOEXEC);
    }
#endif
    return duplicate;
}

/* service.mutex must be held. */
static anbool fitsbin_payload_io_queue_empty_locked(void) {
    return service.queue.head == NULL;
}

/* service.mutex must be held. */
static fitsbin_payload_io_ticket_t*
fitsbin_payload_io_dequeue_locked(void) {
    fitsbin_payload_io_ticket_t* ticket = service.queue.head;

    if (!ticket) {
        return NULL;
    }
    service.queue.head = ticket->next;
    if (!service.queue.head) {
        service.queue.tail = NULL;
    }
    ticket->next = NULL;
    return ticket;
}

/* service.mutex must be held. */
static anbool fitsbin_payload_io_remove_queued_locked(
    fitsbin_payload_io_ticket_t* target) {
    fitsbin_payload_io_ticket_t* previous = NULL;
    fitsbin_payload_io_ticket_t* ticket;

    if (!target) {
        return FALSE;
    }
    ticket = service.queue.head;
    while (ticket) {
        if (ticket == target) {
            if (previous) {
                previous->next = ticket->next;
            } else {
                service.queue.head = ticket->next;
            }
            if (service.queue.tail == ticket) {
                service.queue.tail = previous;
            }
            ticket->next = NULL;
            return TRUE;
        }
        previous = ticket;
        ticket = ticket->next;
    }
    return FALSE;
}

/* service.mutex must be held and ticket must own one admission slot. */
static fitsbin_payload_io_completion_notify_fn
fitsbin_payload_io_terminalize_locked(
    fitsbin_payload_io_ticket_t* ticket,
    fitsbin_payload_io_ticket_state_t state,
    int saved_errno,
    void** completion_opaque) {
    assert(ticket);
    assert(ticket->state == FITSBIN_PAYLOAD_IO_SUBMITTED);
    assert(state != FITSBIN_PAYLOAD_IO_SUBMITTED);
    assert(service.jobs > 0U);
    assert(service.bytes >= ticket->admission_byte_count);

    service.jobs--;
    service.bytes -= ticket->admission_byte_count;
    if (ticket->fd >= 0) {
        close(ticket->fd);
        ticket->fd = -1;
    }
    ticket->saved_errno = saved_errno;
    ticket->state = state;
    pthread_cond_broadcast(&ticket->completion_cv);
    if (service.state == FITSBIN_PAYLOAD_SERVICE_STOPPING &&
        fitsbin_payload_io_queue_empty_locked()) {
        pthread_cond_broadcast(&service.work_cv);
    } else if (!fitsbin_payload_io_queue_empty_locked() &&
               service.active < service.limit) {
        pthread_cond_signal(&service.work_cv);
    }
    return fitsbin_payload_io_completion_acquire_locked(
        completion_opaque);
}

/* service.mutex must be held. */
static fitsbin_payload_io_completion_notify_fn
fitsbin_payload_io_cancel_queued_locked(
    fitsbin_payload_io_ticket_t* ticket,
    void** completion_opaque) {
    if (completion_opaque) {
        *completion_opaque = NULL;
    }
    if (!ticket ||
        ticket->state != FITSBIN_PAYLOAD_IO_SUBMITTED ||
        !__atomic_load_n(
            &ticket->cancel_requested,
            __ATOMIC_ACQUIRE) ||
        !fitsbin_payload_io_remove_queued_locked(ticket)) {
        return NULL;
    }
    return fitsbin_payload_io_terminalize_locked(
        ticket,
        FITSBIN_PAYLOAD_IO_CANCELLED,
        0,
        completion_opaque);
}

/* service.mutex must be held. */
static int fitsbin_payload_io_admit_locked(
    const fitsbin_payload_io_ticket_t* ticket) {
    if (!ticket) {
        return -1;
    }
    if (ticket->admission_byte_count >
        FITSBIN_PAYLOAD_IO_MAX_BYTES) {
        return -1;
    }
    if (service.jobs >= FITSBIN_PAYLOAD_IO_MAX_JOBS ||
        service.bytes >
            FITSBIN_PAYLOAD_IO_MAX_BYTES -
                ticket->admission_byte_count) {
        return 0;
    }
    return 1;
}

/* service.mutex must be held. */
static void fitsbin_payload_io_enqueue_locked(
    fitsbin_payload_io_ticket_t* ticket) {
    if (service.queue.tail) {
        service.queue.tail->next = ticket;
    } else {
        service.queue.head = ticket;
    }
    service.queue.tail = ticket;
}

static fitsbin_payload_io_ticket_t*
fitsbin_payload_io_ticket_alloc(void) {
    fitsbin_payload_io_ticket_t* ticket =
        calloc(1, sizeof(*ticket));
    int status;

    if (!ticket) {
        return NULL;
    }
    ticket->fd = -1;
    status = pthread_cond_init(&ticket->completion_cv, NULL);
    if (status) {
        free(ticket);
        errno = status;
        return NULL;
    }
    return ticket;
}

static void fitsbin_payload_io_ticket_free_storage(
    fitsbin_payload_io_ticket_t* ticket) {
    if (!ticket) {
        return;
    }
    if (ticket->fd >= 0) {
        close(ticket->fd);
    }
    pthread_cond_destroy(&ticket->completion_cv);
    free(ticket);
}

/*
 * Materialize a callback-owned plan into ticket-owned mapped and file spans.
 * The callback runs without payload_io_mutex and has exclusive access to its
 * opaque packet until this ticket reaches a terminal state.
 */
static int fitsbin_payload_io_prepare_planned_ticket(
    fitsbin_payload_io_ticket_t* ticket) {
    size_t range_count = 0U;
    size_t prepared_byte_count = 0U;
    int plan_status;
#if defined(__linux__)
    size_t queued_byte_count = 0U;
#endif

    if (!ticket || !ticket->plan || !ticket->source ||
        !ticket->admission_byte_count) {
        errno = EINVAL;
        return -1;
    }
    if (fitsbin_payload_io_planning_active) {
        errno = EDEADLK;
        return -1;
    }
    fitsbin_payload_io_planning_active = TRUE;
    plan_status = ticket->plan(
        ticket->plan_opaque,
        fitsbin_payload_io_ticket_cancelled,
        ticket,
        ticket->planned_ranges,
        FITSBIN_PREFETCH_RANGE_LIMIT,
        &range_count);
    fitsbin_payload_io_planning_active = FALSE;
    if (fitsbin_payload_io_ticket_cancelled(ticket)) {
        errno = ECANCELED;
        return 1;
    }
    if (plan_status < 0) {
        if (!errno) {
            errno = EIO;
        }
        return -1;
    }
    if (!plan_status) {
        if (range_count) {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    if (plan_status != 1 || !range_count ||
        range_count > FITSBIN_PREFETCH_RANGE_LIMIT) {
        errno = range_count > FITSBIN_PREFETCH_RANGE_LIMIT
            ? E2BIG
            : EINVAL;
        return -1;
    }
    if (fitsbin_prepare_mapped_spans(
            ticket->source,
            ticket->planned_ranges,
            range_count,
            ticket->admission_byte_count,
            ticket->spans,
            FITSBIN_PREFETCH_RANGE_LIMIT,
            &ticket->span_count,
            &prepared_byte_count)) {
        return -1;
    }
    assert(prepared_byte_count <= ticket->admission_byte_count);
#if defined(__linux__)
    if (fitsbin_prepare_mapped_file_spans(
            ticket->source,
            ticket->spans,
            ticket->span_count,
            ticket->admission_byte_count,
            ticket->queued_spans,
            FITSBIN_PREFETCH_RANGE_LIMIT,
            &ticket->queued_span_count,
            &queued_byte_count) ||
        ticket->fd < 0) {
        ticket->queued_span_count = 0U;
    }
#endif
    return 0;
}

static void* fitsbin_payload_io_service_worker(void* opaque) {
    (void)opaque;
    while (1) {
        fitsbin_payload_io_ticket_t* ticket;
        fitsbin_payload_io_completion_notify_fn completion_notify;
        void* completion_opaque = NULL;
        unsigned long long completion_id;
        anbool cancelled;
        size_t work_index;
        int saved_errno = 0;
        int status = 0;

        pthread_mutex_lock(&service.mutex);
        while (fitsbin_payload_io_queue_empty_locked() ||
               service.active >= service.limit) {
            if (fitsbin_payload_io_queue_empty_locked() &&
                service.state == FITSBIN_PAYLOAD_SERVICE_STOPPING) {
                pthread_mutex_unlock(&service.mutex);
                return NULL;
            }
            pthread_cond_wait(
                &service.work_cv,
                &service.mutex);
        }
        ticket = fitsbin_payload_io_dequeue_locked();
        assert(ticket);
        service.active++;
        pthread_mutex_unlock(&service.mutex);

        cancelled = __atomic_load_n(
            &ticket->cancel_requested,
            __ATOMIC_ACQUIRE);
#if defined(MADV_POPULATE_READ)
        if (!ticket->source ||
            !ticket->source->mmap_prefetch_enabled ||
            __atomic_load_n(
                &ticket->source->mmap_prefetch_failed,
                __ATOMIC_ACQUIRE)) {
            saved_errno = ENOTSUP;
            status = -1;
        }
        if (!status && ticket->plan && !cancelled) {
            int plan_status =
                fitsbin_payload_io_prepare_planned_ticket(ticket);

            if (plan_status < 0) {
                saved_errno = errno ? errno : EIO;
                status = -1;
            } else if (plan_status > 0) {
                cancelled = TRUE;
            }
        }
#if defined(__linux__)
            /*
             * Queue the complete, file-offset-ordered page-cache plan before
             * establishing the exact mapped PTEs. RANDOM remains the VMA
             * policy; this explicit bounded queue supplies storage depth
             * without broad speculative readahead. MADV_POPULATE_READ below
             * remains the completion barrier and authoritative readiness
             * test. Queue failure is advisory and retains the old path.
             */
            for (work_index = 0U;
                 !status &&
                     work_index < ticket->queued_span_count &&
                     !cancelled;
                 work_index++) {
                const fitsbin_file_span_t* span =
                    &ticket->queued_spans[work_index];
                size_t span_bytes =
                    (size_t)(span->end - span->begin);
                int queue_status;

                do {
                    queue_status = readahead(
                        ticket->fd, span->begin, span_bytes);
                } while (queue_status && errno == EINTR);
                if (queue_status) {
                    break;
                }
                cancelled = __atomic_load_n(
                    &ticket->cancel_requested,
                    __ATOMIC_ACQUIRE);
            }
#endif
        for (work_index = 0U;
             !status && work_index < ticket->span_count &&
                 !cancelled;
             work_index++) {
            fitsbin_mapped_span_t* span =
                &ticket->spans[work_index];
            size_t span_bytes =
                (size_t)(span->end - span->begin);

            int populate_status;

            if (!span_bytes) {
                errno = EINVAL;
                populate_status = -1;
            } else {
                do {
                    populate_status = madvise(
                        (void*)span->begin,
                        span_bytes,
                        MADV_POPULATE_READ);
                } while (populate_status && errno == EINTR);
            }
            if (populate_status) {
                saved_errno = errno ? errno : EIO;
                status = -1;
                if (ticket->source &&
                    (saved_errno == EINVAL ||
                     saved_errno == ENOSYS ||
                     saved_errno == EFAULT ||
                     saved_errno == EACCES)) {
                    __atomic_store_n(
                        &ticket->source->mmap_prefetch_failed,
                        TRUE,
                        __ATOMIC_RELEASE);
                }
                break;
            }
            cancelled = __atomic_load_n(
                &ticket->cancel_requested,
                __ATOMIC_ACQUIRE);
        }
#else
        saved_errno = ENOTSUP;
        status = -1;
#endif
        pthread_mutex_lock(&service.mutex);
        assert(service.active > 0);
        service.active--;
        if (__atomic_load_n(
            &ticket->cancel_requested,
            __ATOMIC_ACQUIRE) ||
            cancelled) {
            completion_notify =
                fitsbin_payload_io_terminalize_locked(
                    ticket,
                    FITSBIN_PAYLOAD_IO_CANCELLED,
                    0,
                    &completion_opaque);
        } else if (status) {
            completion_notify =
                fitsbin_payload_io_terminalize_locked(
                    ticket,
                    FITSBIN_PAYLOAD_IO_FAILED,
                    saved_errno,
                    &completion_opaque);
        } else {
            completion_notify =
                fitsbin_payload_io_terminalize_locked(
                    ticket,
                    FITSBIN_PAYLOAD_IO_READY,
                    0,
                    &completion_opaque);
        }
        completion_id = ticket->sequence;
        pthread_mutex_unlock(&service.mutex);
        fitsbin_payload_io_completion_dispatch(
            completion_notify,
            completion_opaque,
            completion_id);
    }
    return NULL;
}

int fitsbin_payload_io_service_start(int lane_count) {
    int created = 0;
    int status = 0;

    if (lane_count <= 0) {
        errno = EINVAL;
        return -1;
    }
    lane_count = MIN(lane_count, FITSBIN_PAYLOAD_IO_MAX_LANES);

    pthread_mutex_lock(&service.mutex);
    if (service.state != FITSBIN_PAYLOAD_SERVICE_STOPPED) {
        pthread_mutex_unlock(&service.mutex);
        return 0;
    }
    service.state = FITSBIN_PAYLOAD_SERVICE_RUNNING;
    service.lanes = lane_count;
    while (created < lane_count) {
        status = pthread_create(
            &service.threads[created],
            NULL,
            fitsbin_payload_io_service_worker,
            NULL);
        if (status) {
            break;
        }
        created++;
    }
    if (!status) {
        pthread_mutex_unlock(&service.mutex);
        return 0;
    }
    service.state = FITSBIN_PAYLOAD_SERVICE_STOPPING;
    service.lanes = created;
    pthread_cond_broadcast(&service.work_cv);
    pthread_mutex_unlock(&service.mutex);

    while (created > 0) {
        created--;
        pthread_join(service.threads[created], NULL);
    }
    pthread_mutex_lock(&service.mutex);
    service.state = FITSBIN_PAYLOAD_SERVICE_STOPPED;
    service.lanes = 0;
    pthread_mutex_unlock(&service.mutex);
    errno = status;
    return -1;
}

void fitsbin_payload_io_service_stop(void) {
    int lane_count;
    int lane;

    pthread_mutex_lock(&service.mutex);
    if (service.state == FITSBIN_PAYLOAD_SERVICE_STOPPED) {
        pthread_mutex_unlock(&service.mutex);
        return;
    }
    service.state = FITSBIN_PAYLOAD_SERVICE_STOPPING;
    lane_count = service.lanes;
    pthread_cond_broadcast(&service.work_cv);
    pthread_mutex_unlock(&service.mutex);

    for (lane = 0; lane < lane_count; lane++) {
        pthread_join(service.threads[lane], NULL);
    }

    pthread_mutex_lock(&service.mutex);
    assert(fitsbin_payload_io_queue_empty_locked());
    assert(!service.jobs);
    assert(!service.bytes);
    service.state = FITSBIN_PAYLOAD_SERVICE_STOPPED;
    service.lanes = 0;
    pthread_mutex_unlock(&service.mutex);
}

int fitsbin_payload_io_service_width(void) {
    int lane_count;

    pthread_mutex_lock(&service.mutex);
    lane_count = service.state == FITSBIN_PAYLOAD_SERVICE_RUNNING
        ? service.lanes
        : 0;
    pthread_mutex_unlock(&service.mutex);
    return lane_count;
}

int fitsbin_payload_io_mapped_population_supported(void) {
#if defined(MADV_POPULATE_READ)
    return TRUE;
#else
    return FALSE;
#endif
}

static anbool fitsbin_payload_io_service_available(void) {
    anbool available;

    pthread_mutex_lock(&service.mutex);
    available = service.state == FITSBIN_PAYLOAD_SERVICE_RUNNING;
    pthread_mutex_unlock(&service.mutex);
    return available;
}

static int fitsbin_payload_io_submit_ticket(
    fitsbin_payload_io_ticket_t* ticket,
    fitsbin_payload_io_ticket_t** ticket_out) {
    anbool service_available;
    int admitted = 0;

    pthread_mutex_lock(&service.mutex);
    service_available =
        service.state == FITSBIN_PAYLOAD_SERVICE_RUNNING;
    if (service_available) {
        admitted = fitsbin_payload_io_admit_locked(ticket);
    }
    if (!admitted) {
        pthread_mutex_unlock(&service.mutex);
        fitsbin_payload_io_ticket_free_storage(ticket);
        errno = service_available ? EAGAIN : ENODEV;
        return 0;
    }
    if (admitted < 0) {
        pthread_mutex_unlock(&service.mutex);
        fitsbin_payload_io_ticket_free_storage(ticket);
        errno = E2BIG;
        return 0;
    }
    /*
     * Completion IDs are value tokens that may outlive ticket storage while
     * a notifier is draining. Never recycle an ID after counter exhaustion.
     */
    if (service.next_sequence == ULLONG_MAX) {
        pthread_mutex_unlock(&service.mutex);
        fitsbin_payload_io_ticket_free_storage(ticket);
        errno = EOVERFLOW;
        return -1;
    }
    ticket->sequence = ++service.next_sequence;
    ticket->state = FITSBIN_PAYLOAD_IO_SUBMITTED;
    fitsbin_payload_io_enqueue_locked(ticket);
    service.jobs++;
    service.bytes +=
        ticket->admission_byte_count;
    *ticket_out = ticket;
    pthread_cond_signal(&service.work_cv);
    pthread_mutex_unlock(&service.mutex);
    return 1;
}

int fitsbin_prefetch_ranges_submit(
    fitsbin_t* fb,
    const fitsbin_prefetch_range_t* ranges,
    size_t range_count,
    size_t byte_budget,
    fitsbin_payload_io_ticket_t** ticket_out) {
    fitsbin_payload_io_ticket_t* ticket;
    fitsbin_mapped_span_t
        prepared_spans[FITSBIN_PREFETCH_RANGE_LIMIT];
    size_t prepared_span_count;
    size_t prepared_byte_count;
#if defined(__linux__)
    int fd;
    int duplicate;
    size_t queued_byte_count = 0U;
#endif

    if (fitsbin_payload_io_planning_active) {
        errno = EDEADLK;
        return -1;
    }
    if (!ticket_out) {
        errno = EINVAL;
        return -1;
    }
    *ticket_out = NULL;
    if (!fb || !ranges || !range_count || !byte_budget) {
        errno = EINVAL;
        return -1;
    }
    if (range_count > FITSBIN_PREFETCH_RANGE_LIMIT) {
        errno = E2BIG;
        return -1;
    }
    if (!fb->mmap_prefetch_enabled ||
        __atomic_load_n(
            &fb->mmap_prefetch_failed,
            __ATOMIC_ACQUIRE)) {
        errno = 0;
        return 0;
    }
    if (!fitsbin_payload_io_service_available()) {
        errno = ENODEV;
        return 0;
    }

    if (fitsbin_prepare_mapped_spans(
            fb,
            ranges,
            range_count,
            byte_budget,
            prepared_spans,
            FITSBIN_PREFETCH_RANGE_LIMIT,
            &prepared_span_count,
            &prepared_byte_count)) {
        return -1;
    }
    if (!prepared_span_count) {
        if (!fitsbin_payload_io_service_available()) {
            errno = ENODEV;
            return FITSBIN_PAYLOAD_IO_SUBMIT_UNAVAILABLE;
        }
        errno = 0;
        return FITSBIN_PAYLOAD_IO_SUBMIT_READY;
    }
    ticket = fitsbin_payload_io_ticket_alloc();
    if (!ticket) {
        return -1;
    }
    ticket->source = fb;
    memcpy(
        ticket->spans,
        prepared_spans,
        prepared_span_count * sizeof(*prepared_spans));
    ticket->span_count = prepared_span_count;
    ticket->admission_byte_count = prepared_byte_count;
#if defined(__linux__)
    if (fitsbin_prepare_mapped_file_spans(
            fb,
            ticket->spans,
            ticket->span_count,
            byte_budget,
            ticket->queued_spans,
            FITSBIN_PREFETCH_RANGE_LIMIT,
            &ticket->queued_span_count,
            &queued_byte_count)) {
        ticket->queued_span_count = 0U;
    } else {
        fd = fitsbin_payload_fd_get(fb);
        duplicate = fd >= 0
            ? fitsbin_payload_io_duplicate_fd(fd)
            : -1;
        if (duplicate < 0) {
            ticket->queued_span_count = 0U;
        } else {
            ticket->fd = duplicate;
            ticket->admission_byte_count = MAX(
                ticket->admission_byte_count,
                queued_byte_count);
        }
    }
#endif
    errno = 0;
    return fitsbin_payload_io_submit_ticket(ticket, ticket_out);
}

int fitsbin_prefetch_ranges_planned_submit(
    fitsbin_t* fb,
    fitsbin_payload_io_plan_fn plan,
    void* plan_opaque,
    size_t byte_budget,
    fitsbin_payload_io_ticket_t** ticket_out) {
    fitsbin_payload_io_ticket_t* ticket;
#if defined(__linux__)
    int fd;
    int duplicate;
#endif

    if (fitsbin_payload_io_planning_active) {
        errno = EDEADLK;
        return -1;
    }
    if (!ticket_out) {
        errno = EINVAL;
        return -1;
    }
    *ticket_out = NULL;
    if (!fb || !plan || !byte_budget) {
        errno = EINVAL;
        return -1;
    }
    if (!fb->mmap_prefetch_enabled ||
        __atomic_load_n(
            &fb->mmap_prefetch_failed,
            __ATOMIC_ACQUIRE)) {
        errno = 0;
        return 0;
    }
    if (!fitsbin_payload_io_service_available()) {
        errno = ENODEV;
        return 0;
    }

    ticket = fitsbin_payload_io_ticket_alloc();
    if (!ticket) {
        return -1;
    }
    ticket->source = fb;
    ticket->plan = plan;
    ticket->plan_opaque = plan_opaque;
    ticket->admission_byte_count = byte_budget;
#if defined(__linux__)
    fd = fitsbin_payload_fd_get(fb);
    duplicate = fd >= 0
        ? fitsbin_payload_io_duplicate_fd(fd)
        : -1;
    if (duplicate >= 0) {
        ticket->fd = duplicate;
    }
#endif
    errno = 0;
    return fitsbin_payload_io_submit_ticket(ticket, ticket_out);
}

static anbool fitsbin_payload_io_ticket_terminal(
    fitsbin_payload_io_ticket_state_t state) {
    return state == FITSBIN_PAYLOAD_IO_READY ||
        state == FITSBIN_PAYLOAD_IO_FAILED ||
        state == FITSBIN_PAYLOAD_IO_CANCELLED;
}

/* service.mutex must be held. */
static int fitsbin_payload_io_ticket_result_locked(
    const fitsbin_payload_io_ticket_t* ticket,
    int* result_out,
    int* result_errno) {
    if (!ticket || !result_out || !result_errno) {
        errno = EINVAL;
        return -1;
    }
    if (ticket->state == FITSBIN_PAYLOAD_IO_SUBMITTED) {
        return 0;
    }
    if (ticket->state == FITSBIN_PAYLOAD_IO_READY) {
        *result_out = ticket->span_count
            ? (int)ticket->span_count
            : 1;
        *result_errno = 0;
        return 1;
    }
    if (ticket->state == FITSBIN_PAYLOAD_IO_FAILED) {
        *result_out = -1;
        *result_errno = ticket->saved_errno
            ? ticket->saved_errno
            : EIO;
        return 1;
    }
    if (ticket->state == FITSBIN_PAYLOAD_IO_CANCELLED) {
        *result_out = 0;
        *result_errno = ECANCELED;
        return 1;
    }
    errno = EINVAL;
    return -1;
}

static int fitsbin_payload_io_ticket_finish(
    fitsbin_payload_io_ticket_t** ticket_io,
    int* result_out,
    int result_value,
    int result_errno) {
    fitsbin_payload_io_ticket_t* ticket;

    assert(ticket_io);
    assert(*ticket_io);
    assert(result_out);
    ticket = *ticket_io;
    *result_out = result_value;
    *ticket_io = NULL;
    fitsbin_payload_io_ticket_free_storage(ticket);
    errno = result_errno;
    return 1;
}

int fitsbin_payload_io_ticket_poll_and_destroy(
    fitsbin_payload_io_ticket_t** ticket_io,
    int* result_out) {
    fitsbin_payload_io_ticket_t* ticket;
    int result_errno = 0;
    int result_value = 0;
    int collected;

    if (!ticket_io || !*ticket_io || !result_out) {
        errno = EINVAL;
        return -1;
    }
    ticket = *ticket_io;
    if (!ticket->source) {
        errno = EINVAL;
        return -1;
    }
    *result_out = 0;
    pthread_mutex_lock(&service.mutex);
    if (ticket->owner_draining) {
        pthread_mutex_unlock(&service.mutex);
        errno = EBUSY;
        return -1;
    }
    collected = fitsbin_payload_io_ticket_result_locked(
        ticket,
        &result_value,
        &result_errno);
    pthread_mutex_unlock(&service.mutex);
    if (collected <= 0) {
        return collected;
    }
    return fitsbin_payload_io_ticket_finish(
        ticket_io,
        result_out,
        result_value,
        result_errno);
}

int fitsbin_payload_io_ticket_drain_and_destroy(
    fitsbin_payload_io_ticket_t** ticket_io,
    int* result_out) {
    fitsbin_payload_io_ticket_t* ticket;
    fitsbin_payload_io_completion_notify_fn completion_notify = NULL;
    void* completion_opaque = NULL;
    unsigned long long completion_id;
    int result_errno = 0;
    int result_value = 0;
    int drain_errno;
    int status;

    if (fitsbin_payload_io_planning_active) {
        errno = EDEADLK;
        return -1;
    }
    if (!ticket_io || !*ticket_io || !result_out) {
        errno = EINVAL;
        return -1;
    }
    ticket = *ticket_io;
    *result_out = 0;

    pthread_mutex_lock(&service.mutex);
    if (!ticket->source) {
        pthread_mutex_unlock(&service.mutex);
        errno = EINVAL;
        return -1;
    }
    if (ticket->owner_draining) {
        pthread_mutex_unlock(&service.mutex);
        errno = EBUSY;
        return -1;
    }
    ticket->owner_draining = TRUE;
    completion_id = ticket->sequence;
    if (ticket->state == FITSBIN_PAYLOAD_IO_SUBMITTED) {
        __atomic_store_n(
            &ticket->cancel_requested,
            TRUE,
            __ATOMIC_RELEASE);
        completion_notify =
            fitsbin_payload_io_cancel_queued_locked(
                ticket,
                &completion_opaque);
    }
    pthread_mutex_unlock(&service.mutex);
    fitsbin_payload_io_completion_dispatch(
        completion_notify,
        completion_opaque,
        completion_id);

    pthread_mutex_lock(&service.mutex);
    while (ticket->state == FITSBIN_PAYLOAD_IO_SUBMITTED) {
        status = pthread_cond_wait(
            &ticket->completion_cv,
            &service.mutex);
        if (status) {
            drain_errno = status;
            goto drain_error;
        }
    }
    status = fitsbin_payload_io_ticket_result_locked(
        ticket,
        &result_value,
        &result_errno);
    if (status != 1) {
        drain_errno = status < 0 && errno ? errno : EINVAL;
        goto drain_error;
    }
    pthread_mutex_unlock(&service.mutex);
    return fitsbin_payload_io_ticket_finish(
        ticket_io,
        result_out,
        result_value,
        result_errno);

drain_error:
    ticket->owner_draining = FALSE;
    pthread_cond_broadcast(&ticket->completion_cv);
    pthread_mutex_unlock(&service.mutex);
    errno = drain_errno;
    return -1;
}

int fitsbin_payload_io_ticket_cancel_async(
    fitsbin_payload_io_ticket_t* ticket) {
    fitsbin_payload_io_completion_notify_fn completion_notify = NULL;
    void* completion_opaque = NULL;
    unsigned long long completion_id;
    anbool already_requested;

    if (!ticket) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&service.mutex);
    if (ticket->owner_draining) {
        pthread_mutex_unlock(&service.mutex);
        errno = EBUSY;
        return -1;
    }
    if (fitsbin_payload_io_ticket_terminal(ticket->state)) {
        pthread_mutex_unlock(&service.mutex);
        return 0;
    }
    if (ticket->state != FITSBIN_PAYLOAD_IO_SUBMITTED) {
        pthread_mutex_unlock(&service.mutex);
        errno = EINVAL;
        return -1;
    }
    completion_id = ticket->sequence;
    already_requested = __atomic_exchange_n(
        &ticket->cancel_requested,
        TRUE,
        __ATOMIC_ACQ_REL);
    completion_notify =
        fitsbin_payload_io_cancel_queued_locked(
            ticket,
            &completion_opaque);
    pthread_mutex_unlock(&service.mutex);
    fitsbin_payload_io_completion_dispatch(
        completion_notify,
        completion_opaque,
        completion_id);
    return already_requested ? 0 : 1;
}

unsigned long long fitsbin_payload_io_ticket_completion_id(
    const fitsbin_payload_io_ticket_t* ticket) {
    if (!ticket) {
        return 0ULL;
    }
    return ticket->sequence;
}
