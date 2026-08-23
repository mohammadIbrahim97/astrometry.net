/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "solvedfile.h"
#include "errors.h"
#include "ioutils.h"

#if defined(__APPLE__)
// MacOS 10.3 with gcc 3.3 doesn't have O_SYNC.
#if !defined(O_SYNC)
#define O_SYNC 0
#endif
#endif


int solvedfile_getsize(char* fn) {
    FILE* f;
    off_t end;
    f = fopen(fn, "rb");
    if (!f) {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) ||
        ((end = ftello(f)) == -1)) {
        fprintf(stderr, "Error: seeking to end of file %s: %s\n",
                fn, strerror(errno));
        fclose(f);
        return -1;
    }
    return (int)end;
}

int solvedfile_get(char* fn, int fieldnum) {
    FILE* f;
    unsigned char val;
    off_t end;

    // 1-index field numbers:
    fieldnum--;

    f = fopen(fn, "rb");
    if (!f) {
        // assume it's not solved!
        return 0;
    }
    if (fseek(f, 0, SEEK_END) ||
        ((end = ftello(f)) == -1)) {
        fprintf(stderr, "Error: seeking to end of file %s: %s\n",
                fn, strerror(errno));
        fclose(f);
        return -1;
    }
    if (end <= fieldnum) {
        fclose(f);
        return 0;
    }
    if (fseeko(f, (off_t)fieldnum, SEEK_SET) ||
        (fread(&val, 1, 1, f) != 1) ||
        fclose(f)) {
        fprintf(stderr, "Error: seeking, reading, or closing file %s: %s\n",
                fn, strerror(errno));
        fclose(f);
        return -1;
    }
    return val;
}

// lastfield = 0 for no limit.
static il* solvedfile_getall_val(char* fn, int firstfield, int lastfield, int maxfields, int val) {
    FILE* f;
    off_t end;
    int fields = 0;
    il* list;
    int i;
    unsigned char* map;

    list = il_new(256);

    f = fopen(fn, "rb");
    if (!f) {
        // if file doesn't exist, assume no fields are solved.
        if (val == 0) {
            for (i=firstfield; i<=lastfield; i++) {
                il_append(list, i);
                fields++;
                if (fields == maxfields)
                    break;
            }
        }
        return list;
    }

    if (fseek(f, 0, SEEK_END) ||
        ((end = ftello(f)) == -1)) {
        fprintf(stderr, "Error: seeking to end of file %s: %s\n",
                fn, strerror(errno));
        fclose(f);
        il_free(list);
        return NULL;
    }
    // 1-index
    firstfield--;
    lastfield--;
    if (end <= firstfield) {
        fclose(f);
        return list;
    }

    map = mmap(NULL, end, PROT_READ, MAP_SHARED, fileno(f), 0);
    fclose(f);
    if (map == MAP_FAILED) {
        fprintf(stderr, "Error: couldn't mmap file %s: %s\n", fn, strerror(errno));
        il_free(list);
        return NULL;
    }

    for (i=firstfield; ((lastfield == -1) || (i<=lastfield)) && (i < end); i++) {
        if (map[i] == val) {
            // 1-index
            il_append(list, i+1);
            if (il_size(list) == maxfields)
                break;
        }
    }

    munmap(map, end);

    if (val == 0) {
        // fields larger than the file size are unsolved.
        for (i=end; i<=lastfield; i++) {
            if (il_size(list) == maxfields)
                break;
            // 1-index
            il_append(list, i+1);
        }
    }
    return list;
}

il* solvedfile_getall(char* fn, int firstfield, int lastfield, int maxfields) {
    return solvedfile_getall_val(fn, firstfield, lastfield, maxfields, 0);
}

il* solvedfile_getall_solved(char* fn, int firstfield, int lastfield, int maxfields) {
    return solvedfile_getall_val(fn, firstfield, lastfield, maxfields, 1);
}

static int solvedfile_lock(
    const char* fn,
    char** target_out,
    int* lock_out,
    mode_t* lock_mode_out) {
    struct stat info;
    struct stat link_info;
    char* lock_path = NULL;
    char* target = NULL;
    int lock_file = -1;

    target = realpath(fn, NULL);
    if (!target) {
        if (errno != ENOENT) {
            goto fail;
        }
        if (!lstat(fn, &link_info) && S_ISLNK(link_info.st_mode)) {
            errno = ENOENT;
            goto fail;
        }
        target = strdup(fn);
        if (!target) {
            goto fail;
        }
    }
    lock_path = malloc(strlen(target) + sizeof(".lock"));
    if (!lock_path) {
        goto fail;
    }
    sprintf(lock_path, "%s.lock", target);
    lock_file = open(
        lock_path,
        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
    if (lock_file < 0 || fstat(lock_file, &info)) {
        goto fail;
    }
    if (!S_ISREG(info.st_mode)) {
        errno = EINVAL;
        goto fail;
    }
    if (flock(lock_file, LOCK_EX)) {
        goto fail;
    }
    free(lock_path);
    *target_out = target;
    *lock_out = lock_file;
    if (lock_mode_out) {
        *lock_mode_out = info.st_mode & 0777;
    }
    return 0;

 fail:
    if (lock_file >= 0) {
        close(lock_file);
    }
    free(lock_path);
    free(target);
    return -1;
}

static mode_t solvedfile_creation_mode(void) {
    FILE* status;
    char line[128];
    unsigned int mask;

    status = fopen("/proc/self/status", "r");
    if (status) {
        while (fgets(line, sizeof(line), status)) {
            if (sscanf(line, "Umask:\t%o", &mask) == 1) {
                fclose(status);
                return (mode_t)(0666U & ~mask);
            }
        }
        fclose(status);
    }
    /* A conservative fallback for systems without Linux procfs. */
    return S_IRUSR | S_IWUSR;
}

static int solvedfile_setsize_unlocked(char* fn, int sz) {
    int f;
    unsigned char val;
    off_t off;
    f = open(fn, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
    if (f == -1) {
        fprintf(stderr, "Error: failed to open file %s for writing: %s\n",
                fn, strerror(errno));
        return -1;
    }
    off = lseek(f, 0, SEEK_END);
    if (off == -1) {
        fprintf(stderr, "Error: failed to lseek() to end of file %s: %s\n", fn, strerror(errno));
        close(f);
        return -1;
    }
    // this gives you the offset one past the end of the file.
    if (off < sz) {
        // pad.
        int npad = sz - off;
        int i;
        val = 0;
        for (i=0; i<npad; i++)
            if (write(f, &val, 1) != 1) {
                fprintf(stderr, "Error: failed to write padding to file %s: %s\n",
                        fn, strerror(errno));
                close(f);
                return -1;
            }
    }
    if (close(f)) {
        fprintf(stderr, "Error closing file %s: %s\n", fn, strerror(errno));
        return -1;
    }
    return 0;
}

int solvedfile_setsize(char* fn, int sz) {
    char* target = NULL;
    int lock_file = -1;
    int rtn;

    if (solvedfile_lock(fn, &target, &lock_file, NULL)) {
        return -1;
    }
    rtn = solvedfile_setsize_unlocked(target, sz);
    close(lock_file);
    free(target);
    return rtn;
}

int solvedfile_set_array(char* fn, anbool* vals, int N) {
    char* target = NULL;
    int f;
    int lock_file = -1;
    unsigned char val;
    int i;
    int rtn = -1;

    if (solvedfile_lock(fn, &target, &lock_file, NULL)) {
        return -1;
    }
    if (solvedfile_setsize_unlocked(target, N)) {
        goto cleanup;
    }

    // (file mode 666; umask will modify this, if set).
    f = open(target, O_WRONLY | O_CREAT | O_SYNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
    if (f == -1) {
        fprintf(stderr, "Error: failed to open file %s for writing: %s\n",
                target, strerror(errno));
        goto cleanup;
    }
    val = 1;
    for (i=0; i<N; i++) {
        if (!vals[i])
            continue;
        if ((lseek(f, (off_t)i, SEEK_SET) == -1) ||
            (write(f, &val, 1) != 1)) {
            fprintf(stderr, "Error: seeking or writing file %s: %s\n",
                    target, strerror(errno));
            close(f);
            goto cleanup;
        }
    }
    if (close(f)) {
        fprintf(stderr, "Error closing file %s: %s\n",
                target, strerror(errno));
        goto cleanup;
    }
    rtn = 0;

 cleanup:
    close(lock_file);
    free(target);
    return rtn;
}

int solvedfile_set_file(char* fn, anbool* vals, int N) {
    char* target = NULL;
    FILE* f;
    int lock_file = -1;
    int i;
    int rtn = -1;

    // Ensure the array contains values 0, 1.
    for (i=0; i<N; i++)
        if (vals[i]) vals[i] = TRUE;
        else vals[i] = FALSE;

    if (solvedfile_lock(fn, &target, &lock_file, NULL)) {
        return -1;
    }
    f = fopen(target, "wb");
    if (!f) {
        SYSERROR("Failed to open file \"%s\" for writing", target);
        goto cleanup;
    }
    if (fwrite(vals, 1, N, f) != N) {
        SYSERROR("Failed to write solved file \"%s\"", target);
        fclose(f);
        goto cleanup;
    }
    if (fclose(f)) {
        SYSERROR("Failed to close solved file \"%s\"", target);
        goto cleanup;
    }
    rtn = 0;

 cleanup:
    close(lock_file);
    free(target);
    return rtn;
}

int solvedfile_set(char* fn, int fieldnum) {
    char* target = NULL;
    unsigned char val;
    off_t off;
    int f = -1;
    int lock_file = -1;
    int rtn = -1;

    if (fieldnum < 1) {
        errno = EINVAL;
        return -1;
    }
    if (solvedfile_lock(fn, &target, &lock_file, NULL)) {
        return -1;
    }

    fieldnum--;
    f = open(
        target,
        O_WRONLY | O_CREAT | O_SYNC,
        S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
    if (f < 0) {
        fprintf(stderr, "Error: failed to open file %s for writing: %s\n",
                target, strerror(errno));
        goto cleanup;
    }
    off = lseek(f, 0, SEEK_END);
    if (off == -1) {
        fprintf(stderr, "Error: failed to lseek() to end of file %s: %s\n",
                target, strerror(errno));
        goto cleanup;
    }
    if (off < fieldnum) {
        int npad = fieldnum - off;
        int i;

        val = 0;
        for (i = 0; i < npad; i++) {
            if (write(f, &val, 1) != 1) {
                fprintf(stderr, "Error: failed to write padding to file %s: %s\n",
                        target, strerror(errno));
                goto cleanup;
            }
        }
    }
    val = 1;
    if (lseek(f, (off_t)fieldnum, SEEK_SET) == -1 ||
        write(f, &val, 1) != 1) {
        fprintf(stderr, "Error: seeking or writing file %s: %s\n",
                target, strerror(errno));
        goto cleanup;
    }
    if (close(f)) {
        f = -1;
        fprintf(stderr, "Error closing file %s: %s\n",
                target, strerror(errno));
        goto cleanup;
    }
    f = -1;
    rtn = 0;

 cleanup:
    if (f >= 0) {
        close(f);
    }
    close(lock_file);
    free(target);
    return rtn;
}

int solvedfile_set_list_atomic(char* fn, il* fields) {
    struct stat before;
    struct stat after;
    unsigned char* values = NULL;
    char* parent = NULL;
    char* target = NULL;
    char* temporary = NULL;
    mode_t marker_mode = 0;
    size_t existing_size = 0;
    size_t output_size = 0;
    size_t offset = 0;
    int directory = -1;
    int input = -1;
    int lock_file = -1;
    int output = -1;
    int i;
    int rtn = -1;

    if (!fn || !fields) {
        errno = EINVAL;
        return -1;
    }
    if (!il_size(fields)) {
        return 0;
    }
    for (i = 0; i < il_size(fields); i++) {
        int fieldnum = il_get(fields, i);

        if (fieldnum < 1) {
            errno = EINVAL;
            goto cleanup;
        }
        if ((size_t)fieldnum > output_size) {
            output_size = (size_t)fieldnum;
        }
    }

    if (solvedfile_lock(fn, &target, &lock_file, &marker_mode)) {
        goto cleanup;
    }

    input = open(target, O_RDONLY | O_CLOEXEC);
    if (input >= 0) {
        if (fstat(input, &before)) {
            goto cleanup;
        }
        if (!S_ISREG(before.st_mode) || before.st_size < 0 ||
            (uintmax_t)before.st_size > (uintmax_t)SIZE_MAX) {
            errno = EINVAL;
            goto cleanup;
        }
        existing_size = (size_t)before.st_size;
        marker_mode = before.st_mode & 07777;
        if (existing_size > output_size) {
            output_size = existing_size;
        }
    } else {
        if (errno != ENOENT) {
            goto cleanup;
        }
        marker_mode = solvedfile_creation_mode();
    }

    values = calloc(output_size, 1U);
    if (!values) {
        goto cleanup;
    }
    while (input >= 0 && offset < existing_size) {
        ssize_t count = read(input, values + offset, existing_size - offset);

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            if (!count) {
                errno = EIO;
            }
            goto cleanup;
        }
        offset += (size_t)count;
    }
    if (input >= 0 &&
        (fstat(input, &after) ||
         before.st_dev != after.st_dev ||
         before.st_ino != after.st_ino ||
         before.st_size != after.st_size ||
         before.st_mtime != after.st_mtime)) {
        errno = EAGAIN;
        goto cleanup;
    }
    for (i = 0; i < il_size(fields); i++) {
        values[il_get(fields, i) - 1] = 1;
    }

    temporary = malloc(strlen(target) + sizeof(".tmp.XXXXXX"));
    if (!temporary) {
        goto cleanup;
    }
    sprintf(temporary, "%s.tmp.XXXXXX", target);
    output = mkstemp(temporary);
    if (output < 0) {
        goto cleanup;
    }
    if (fchmod(output, marker_mode)) {
        goto cleanup;
    }
    offset = 0;
    while (offset < output_size) {
        ssize_t count = write(output, values + offset, output_size - offset);

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            if (!count) {
                errno = EIO;
            }
            goto cleanup;
        }
        offset += (size_t)count;
    }
    if (fsync(output)) {
        goto cleanup;
    }
    if (close(output)) {
        output = -1;
        goto cleanup;
    }
    output = -1;
    parent = dirname_safe(target);
    if (!parent) {
        goto cleanup;
    }
    directory = open(
        parent,
        O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (directory < 0 || fsync(directory)) {
        goto cleanup;
    }
    if (rename(temporary, target)) {
        goto cleanup;
    }
    temporary[0] = '\0';
    if (fsync(directory)) {
        fprintf(stderr,
                "Warning: failed to synchronize solvedfile directory %s: %s\n",
                parent,
                strerror(errno));
    }
    rtn = 0;

 cleanup:
    if (directory >= 0) {
        close(directory);
    }
    if (input >= 0) {
        close(input);
    }
    if (output >= 0) {
        close(output);
    }
    if (temporary && temporary[0]) {
        unlink(temporary);
    }
    if (lock_file >= 0) {
        close(lock_file);
    }
    free(parent);
    free(temporary);
    free(target);
    free(values);
    return rtn;
}
