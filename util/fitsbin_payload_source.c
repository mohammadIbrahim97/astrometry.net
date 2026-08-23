/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "fitsbin.h"
#include "fitsbin_internal.h"
#include "ioutils.h"

int fitsbin_payload_fd_get(fitsbin_t* fb) {
    int fd;
    int opened;
    int expected;
    int advice_rc;
    int saved_errno;
    struct stat actual;

    if (!fb || !fb->payload_fd_initialized ||
        !fb->filename ||
        !fb->open_file_stat_valid) {
        errno = ENOTSUP;
        return -1;
    }
    fd = __atomic_load_n(
        &fb->payload_fd,
        __ATOMIC_ACQUIRE);
    if (fd >= 0) {
        return fd;
    }
    if (__atomic_load_n(
            &fb->payload_fd_failed,
            __ATOMIC_ACQUIRE)) {
        errno = ENOTSUP;
        return -1;
    }

#ifdef O_CLOEXEC
    opened = open(fb->filename, O_RDONLY | O_CLOEXEC);
#else
    opened = open(fb->filename, O_RDONLY);
#endif
    if (opened < 0) {
        goto fail;
    }
    if (fstat(opened, &actual) ||
        !stat_file_identity_equal(
            &fb->open_file_stat,
            &actual)) {
        close(opened);
        errno = ESTALE;
        goto fail;
    }
#if defined(POSIX_FADV_RANDOM)
    advice_rc = posix_fadvise(
        opened,
        0,
        0,
        POSIX_FADV_RANDOM);
    if (advice_rc) {
        close(opened);
        errno = advice_rc;
        goto fail;
    }
#else
    close(opened);
    errno = ENOTSUP;
    goto fail;
#endif

    expected = -1;
    if (!__atomic_compare_exchange_n(
            &fb->payload_fd,
            &expected,
            opened,
            FALSE,
            __ATOMIC_RELEASE,
            __ATOMIC_ACQUIRE)) {
        close(opened);
        if (expected >= 0) {
            return expected;
        }
        errno = EIO;
        goto fail;
    }
    return opened;

fail:
    saved_errno = errno;
    fd = __atomic_load_n(
        &fb->payload_fd,
        __ATOMIC_ACQUIRE);
    if (fd >= 0) {
        return fd;
    }
    __atomic_store_n(
        &fb->payload_fd_failed,
        TRUE,
        __ATOMIC_RELEASE);
    errno = saved_errno;
    return -1;
}

int fitsbin_close_payload_fd(fitsbin_t* fb) {
    int fd;

    if (!fb || !fb->payload_fd_initialized) {
        return 0;
    }
    fd = __atomic_exchange_n(
        &fb->payload_fd,
        -1,
        __ATOMIC_ACQ_REL);
    if (fd >= 0 && close(fd)) {
        return -1;
    }
    __atomic_store_n(
        &fb->payload_fd_failed,
        FALSE,
        __ATOMIC_RELEASE);
    return 0;
}
