/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "astrometry/engine.h"
#include "astrometry/solvedfile.h"
#include "astrometry/solverutils.h"
#include "engine_internal.h"
#include "onefield_internal.h"

static int failures = 0;

#define CHECK(expression)                                                \
    do {                                                                 \
        if (!(expression)) {                                             \
            fprintf(stderr, "CHECK failed at %s:%i: %s\n",             \
                    __FILE__, __LINE__, #expression);                    \
            failures++;                                                  \
        }                                                                \
    } while (0)

static anbool close_enough(double left, double right) {
    return fabs(left - right) < 1.0e-12;
}

static void bounded_then_open_resets_endobj(void) {
    job_t job;
    solver_t solver;
    engine_pass_cursor_t cursor;
    engine_pass_t pass;

    memset(&job, 0, sizeof(job));
    memset(&solver, 0, sizeof(solver));
    job.depths = il_new(4);
    job.scales = dl_new(2);
    CHECK(job.depths != NULL);
    CHECK(job.scales != NULL);
    CHECK(parse_depth_string(job.depths, "5-10 30-") == 0);
    dl_append(job.scales, 0.5);
    dl_append(job.scales, 1.0);

    solver.endobj = 777;
    engine_pass_cursor_init(&cursor);
    CHECK(engine_pass_cursor_next(
        &job, 7.5, 9.5, &cursor, &pass));
    CHECK(pass.ordinal == 0U);
    engine_pass_apply(&solver, &pass);
    CHECK(solver.startobj == 4);
    CHECK(solver.endobj == 10);

    CHECK(engine_pass_cursor_next(
        &job, 7.5, 9.5, &cursor, &pass));
    CHECK(pass.ordinal == 1U);
    engine_pass_apply(&solver, &pass);
    CHECK(solver.startobj == 29);
    CHECK(solver.endobj == 0);
    CHECK(!engine_pass_cursor_next(
        &job, 7.5, 9.5, &cursor, &pass));

    il_free(job.depths);
    dl_free(job.scales);
}

static void range_major_scale_minor_order(void) {
    static const int expected_start[] = {
        4, 4, 4, 1, 1, 1, 29, 29, 29
    };
    static const int expected_end[] = {
        10, 10, 10, 3, 3, 3, 0, 0, 0
    };
    static const double expected_lower[] = {
        0.5, 2.0, 7.5, 0.5, 2.0, 7.5, 0.5, 2.0, 7.5
    };
    static const double expected_upper[] = {
        1.0, 4.0, 9.5, 1.0, 4.0, 9.5, 1.0, 4.0, 9.5
    };
    job_t job;
    solver_t solver;
    engine_pass_cursor_t cursor;
    engine_pass_t pass;
    size_t ordinal;

    memset(&job, 0, sizeof(job));
    memset(&solver, 0, sizeof(solver));
    job.depths = il_new(6);
    job.scales = dl_new(6);
    CHECK(job.depths != NULL);
    CHECK(job.scales != NULL);
    CHECK(parse_depth_string(job.depths, "5-10 2-3 30-") == 0);
    dl_append(job.scales, 0.5);
    dl_append(job.scales, 1.0);
    dl_append(job.scales, 2.0);
    dl_append(job.scales, 4.0);
    dl_append(job.scales, 0.0);
    dl_append(job.scales, 0.0);

    engine_pass_cursor_init(&cursor);
    for (ordinal = 0U; ordinal < 9U; ordinal++) {
        CHECK(engine_pass_cursor_next(
            &job, 7.5, 9.5, &cursor, &pass));
        CHECK(pass.ordinal == ordinal);
        CHECK(pass.depth_index == ordinal / 3U);
        CHECK(pass.scale_index == ordinal % 3U);
        CHECK(pass.startobj == expected_start[ordinal]);
        CHECK(pass.endobj == expected_end[ordinal]);
        CHECK(close_enough(
            pass.funits_lower, expected_lower[ordinal]));
        CHECK(close_enough(
            pass.funits_upper, expected_upper[ordinal]));

        solver.startobj = -1;
        solver.endobj = -1;
        engine_pass_apply(&solver, &pass);
        CHECK(solver.startobj == expected_start[ordinal]);
        CHECK(solver.endobj == expected_end[ordinal]);
        CHECK(close_enough(
            solver.funits_lower, expected_lower[ordinal]));
        CHECK(close_enough(
            solver.funits_upper, expected_upper[ordinal]));
    }
    CHECK(!engine_pass_cursor_next(
        &job, 7.5, 9.5, &cursor, &pass));

    il_free(job.depths);
    dl_free(job.scales);
}

static void primary_wall_and_optional_cpu_policy(void) {
    engine_limit_policy_t limits;
    engine_t* engine;

    engine = engine_new();
    CHECK(engine != NULL);
    if (engine) {
        CHECK(close_enough(engine->walllimit, 300.0));
        CHECK(close_enough(engine->cpulimit, 0.0));
        engine_free(engine);
    }

    {
        FILE* config = tmpfile();

        CHECK(config != NULL);
        if (config) {
            engine = engine_new();
            CHECK(engine != NULL);
            CHECK(fputs("walllimit 240\ncpulimit 0\n", config) >= 0);
            rewind(config);
            if (engine) {
                CHECK(engine_parse_config_file_stream(engine, config) == 0);
                CHECK(close_enough(engine->walllimit, 240.0));
                CHECK(close_enough(engine->cpulimit, 0.0));
                engine_free(engine);
            }
            fclose(config);
        }
    }

    engine_limit_policy_resolve(0.0, 300.0, 0.0, 0.0, &limits);
    CHECK(close_enough(limits.wall_seconds, 300.0));
    CHECK(close_enough(limits.cpu_seconds, 0.0));
    CHECK(!limits.wall_from_job);
    CHECK(!limits.wall_job_clamped);
    CHECK(!limits.cpu_from_job);

    engine_limit_policy_resolve(60.0, 300.0, 720.0, 0.0, &limits);
    CHECK(close_enough(limits.wall_seconds, 60.0));
    CHECK(close_enough(limits.cpu_seconds, 720.0));
    CHECK(limits.wall_from_job);
    CHECK(!limits.wall_job_clamped);
    CHECK(limits.cpu_from_job);

    engine_limit_policy_resolve(600.0, 300.0, 720.0, 120.0, &limits);
    CHECK(close_enough(limits.wall_seconds, 300.0));
    CHECK(close_enough(limits.cpu_seconds, 720.0));
    CHECK(!limits.wall_from_job);
    CHECK(limits.wall_job_clamped);
    CHECK(limits.cpu_from_job);

    engine_limit_policy_resolve(600.0, 0.0, 0.0, 120.0, &limits);
    CHECK(close_enough(limits.wall_seconds, 600.0));
    CHECK(close_enough(limits.cpu_seconds, 120.0));
    CHECK(limits.wall_from_job);
    CHECK(!limits.wall_job_clamped);
    CHECK(!limits.cpu_from_job);
}

static void job_result_outcome_precedence(void) {
    onefield_t onefield;
    engine_job_result_t result;

    memset(&onefield, 0, sizeof(onefield));
    engine_job_result_from_onefield(&result, 0, &onefield);
    CHECK(result.engine_rc == 0);
    CHECK(result.outcome == ENGINE_JOB_OUTCOME_UNSOLVED);
    CHECK(strcmp(engine_job_outcome_string(result.outcome), "UNSOLVED") == 0);

    onefield.hit_total_cpulimit = TRUE;
    engine_job_result_from_onefield(&result, 0, &onefield);
    CHECK(result.outcome == ENGINE_JOB_OUTCOME_CPU_LIMIT);
    CHECK(result.cpu_limit);

    onefield.hit_timelimit = TRUE;
    engine_job_result_from_onefield(&result, 0, &onefield);
    CHECK(result.outcome == ENGINE_JOB_OUTCOME_WALL_LIMIT);
    CHECK(result.wall_limit);
    CHECK(result.cpu_limit);

    onefield.cancelled = TRUE;
    engine_job_result_from_onefield(&result, 0, &onefield);
    CHECK(result.outcome == ENGINE_JOB_OUTCOME_CANCELLED);

    onefield.single_field_solved = TRUE;
    engine_job_result_from_onefield(&result, 0, &onefield);
    CHECK(result.outcome == ENGINE_JOB_OUTCOME_SOLVED);

    onefield.solver_failed = TRUE;
    engine_job_result_from_onefield(&result, 0, &onefield);
    CHECK(result.outcome == ENGINE_JOB_OUTCOME_ERROR);
    CHECK(result.execution_error);

    memset(&onefield, 0, sizeof(onefield));
    onefield.solved_fields_pending = il_new(2);
    CHECK(onefield.solved_fields_pending != NULL);
    if (onefield.solved_fields_pending) {
        il_append(onefield.solved_fields_pending, 2);
        engine_job_result_from_onefield(&result, 0, &onefield);
        CHECK(result.outcome == ENGINE_JOB_OUTCOME_SOLVED);
        CHECK(result.solved);
        il_free(onefield.solved_fields_pending);
    }

    onefield_init(&onefield);
    CHECK(onefield.solved_fields_pending != NULL);
    if (onefield.solved_fields_pending) {
        onefield_internal_solved_field(&onefield, 2);
        CHECK(onefield.any_field_solved);
        onefield_clear_solutions(&onefield);
        CHECK(il_size(onefield.solved_fields_pending) == 0);
        engine_job_result_from_onefield(&result, 0, &onefield);
        CHECK(result.outcome == ENGINE_JOB_OUTCOME_SOLVED);
        CHECK(result.solved);
    }
    onefield_cleanup(&onefield);

    memset(&onefield, 0, sizeof(onefield));
    engine_job_result_from_onefield(&result, -7, &onefield);
    CHECK(result.engine_rc == -7);
    CHECK(result.outcome == ENGINE_JOB_OUTCOME_ERROR);
    CHECK(result.execution_error);

    engine_job_result_from_onefield(&result, 0, NULL);
    CHECK(result.outcome == ENGINE_JOB_OUTCOME_ERROR);
    CHECK(result.execution_error);

    CHECK(strcmp(engine_job_outcome_string(ENGINE_JOB_OUTCOME_SOLVED),
                 "SOLVED") == 0);
    CHECK(strcmp(engine_job_outcome_string(ENGINE_JOB_OUTCOME_WALL_LIMIT),
                 "WALL_LIMIT") == 0);
    CHECK(strcmp(engine_job_outcome_string(ENGINE_JOB_OUTCOME_CPU_LIMIT),
                 "CPU_LIMIT") == 0);
    CHECK(strcmp(engine_job_outcome_string(ENGINE_JOB_OUTCOME_CANCELLED),
                 "CANCELLED") == 0);
    CHECK(strcmp(engine_job_outcome_string(ENGINE_JOB_OUTCOME_ERROR),
                 "ERROR") == 0);
}

static void obsolete_markers_preserve_terminal_state(void) {
    char solved_path[] = "/tmp/proi26-engine-solved-XXXXXX";
    char solved_lock_path[PATH_MAX];
    char cancel_path[] = "/tmp/proi26-engine-cancel-XXXXXX";
    engine_job_result_t result;
    onefield_t onefield;
    int fd;

    fd = mkstemp(solved_path);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(close(fd) == 0);
        CHECK(solvedfile_set(solved_path, 1) == 0);

        onefield_init(&onefield);
        onefield_add_field(&onefield, 1);
        onefield_set_solvedin_file(&onefield, solved_path);
        CHECK(onefield_is_run_obsolete(&onefield, &onefield.solver));
        engine_job_result_from_onefield(&result, 0, &onefield);
        CHECK(result.outcome == ENGINE_JOB_OUTCOME_SOLVED);
        onefield_cleanup(&onefield);
        snprintf(solved_lock_path, sizeof(solved_lock_path), "%s.lock", solved_path);
        CHECK(unlink(solved_path) == 0);
        CHECK(unlink(solved_lock_path) == 0);
    }

    fd = mkstemp(cancel_path);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(close(fd) == 0);

        onefield_init(&onefield);
        onefield_add_field(&onefield, 1);
        onefield_set_cancel_file(&onefield, cancel_path);
        CHECK(onefield_is_run_obsolete(&onefield, &onefield.solver));
        engine_job_result_from_onefield(&result, 0, &onefield);
        CHECK(result.outcome == ENGINE_JOB_OUTCOME_CANCELLED);
        onefield_cleanup(&onefield);
        CHECK(unlink(cancel_path) == 0);
    }
}

static void solved_marker_batch_is_atomic(void) {
    char solved_path[] = "/tmp/proi26-solved-batch-XXXXXX";
    char mode_path[] = "/tmp/proi26-solved-mode-XXXXXX";
    char concurrent_path[] = "/tmp/proi26-solved-concurrent-XXXXXX";
    char failure_dir[] = "/tmp/proi26-solved-failure-XXXXXX";
    char failure_path[256];
    char lock_path[512];
    struct stat info;
    mode_t old_umask;
    il* fields;
    int gate[2];
    int status;
    int fd;
    pid_t first;
    pid_t second;

    fd = mkstemp(solved_path);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(close(fd) == 0);
        CHECK(solvedfile_set(solved_path, 1) == 0);
        fields = il_new(2);
        CHECK(fields != NULL);
        if (fields) {
            il_append(fields, 2);
            il_append(fields, 4);
            CHECK(solvedfile_set_list_atomic(solved_path, fields) == 0);
            CHECK(solvedfile_get(solved_path, 1) == 1);
            CHECK(solvedfile_get(solved_path, 2) == 1);
            CHECK(solvedfile_get(solved_path, 3) == 0);
            CHECK(solvedfile_get(solved_path, 4) == 1);
            il_free(fields);
        }
        snprintf(lock_path, sizeof(lock_path), "%s.lock", solved_path);
        CHECK(unlink(solved_path) == 0);
        CHECK(unlink(lock_path) == 0);
    }

    fd = mkstemp(mode_path);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(close(fd) == 0);
        CHECK(unlink(mode_path) == 0);
        fields = il_new(1);
        CHECK(fields != NULL);
        if (fields) {
            il_append(fields, 1);
            old_umask = umask(0027);
            CHECK(solvedfile_set_list_atomic(mode_path, fields) == 0);
            umask(old_umask);
            CHECK(stat(mode_path, &info) == 0);
            CHECK((info.st_mode & 0777) == 0640);
            il_free(fields);
        }
        snprintf(lock_path, sizeof(lock_path), "%s.lock", mode_path);
        CHECK(unlink(mode_path) == 0);
        fields = il_new(1);
        CHECK(fields != NULL);
        if (fields) {
            il_append(fields, 1);
            old_umask = umask(0077);
            CHECK(solvedfile_set_list_atomic(mode_path, fields) == 0);
            umask(old_umask);
            CHECK(stat(mode_path, &info) == 0);
            CHECK((info.st_mode & 0777) == 0600);
            il_free(fields);
        }
        CHECK(unlink(mode_path) == 0);
        CHECK(unlink(lock_path) == 0);
    }

    fd = mkstemp(concurrent_path);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(close(fd) == 0);
        CHECK(unlink(concurrent_path) == 0);
        CHECK(pipe(gate) == 0);
        first = fork();
        if (first == 0) {
            unsigned char release;
            int child_status = 1;

            close(gate[1]);
            if (read(gate[0], &release, 1) == 1) {
                child_status = solvedfile_set(concurrent_path, 1) ? 1 : 0;
            }
            close(gate[0]);
            _exit(child_status);
        }
        second = fork();
        if (second == 0) {
            unsigned char release;
            il* child_fields = il_new(1);
            int child_status = 1;

            close(gate[1]);
            if (read(gate[0], &release, 1) == 1 && child_fields) {
                il_append(child_fields, 2);
                child_status = solvedfile_set_list_atomic(
                    concurrent_path, child_fields) ? 1 : 0;
            }
            il_free(child_fields);
            close(gate[0]);
            _exit(child_status);
        }
        close(gate[0]);
        CHECK(write(gate[1], "12", 2) == 2);
        CHECK(close(gate[1]) == 0);
        CHECK(first > 0);
        CHECK(second > 0);
        if (first > 0) {
            CHECK(waitpid(first, &status, 0) == first);
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        }
        if (second > 0) {
            CHECK(waitpid(second, &status, 0) == second);
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        }
        CHECK(solvedfile_get(concurrent_path, 1) == 1);
        CHECK(solvedfile_get(concurrent_path, 2) == 1);
        snprintf(lock_path, sizeof(lock_path), "%s.lock", concurrent_path);
        CHECK(unlink(concurrent_path) == 0);
        CHECK(unlink(lock_path) == 0);
    }

    if (mkdtemp(failure_dir)) {
        snprintf(failure_path, sizeof(failure_path), "%s/markers", failure_dir);
        CHECK(solvedfile_set(failure_path, 1) == 0);
        fields = il_new(1);
        CHECK(fields != NULL);
        if (fields) {
            il_append(fields, 2);
            CHECK(chmod(failure_dir, S_IRUSR | S_IXUSR) == 0);
            CHECK(solvedfile_set_list_atomic(failure_path, fields) != 0);
            CHECK(solvedfile_get(failure_path, 1) == 1);
            CHECK(solvedfile_get(failure_path, 2) == 0);
            CHECK(chmod(failure_dir, S_IRWXU) == 0);
            il_free(fields);
        }
        snprintf(lock_path, sizeof(lock_path), "%s.lock", failure_path);
        CHECK(unlink(failure_path) == 0);
        CHECK(unlink(lock_path) == 0);
        CHECK(rmdir(failure_dir) == 0);
    } else {
        CHECK(FALSE);
    }
}

int main(void) {
    bounded_then_open_resets_endobj();
    range_major_scale_minor_order();
    primary_wall_and_optional_cpu_policy();
    job_result_outcome_precedence();
    obsolete_markers_preserve_terminal_state();
    solved_marker_batch_is_atomic();
    if (failures) {
        fprintf(stderr, "ENGINE_PASS_TEST_FAILED failures=%i\n", failures);
        return 1;
    }
    printf("ENGINE_PASS_TEST_OK cases=6 ordered_passes=9\n");
    return 0;
}
