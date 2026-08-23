/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */

#ifndef SOLVER_CODEKD_TEST_PRIVATE_H
#define SOLVER_CODEKD_TEST_PRIVATE_H

#include <stddef.h>

#define SOLVER_CODEKD_TEST_WINDOW_CAPACITY 4U

typedef struct solver_codekd_test_window_result {
    size_t window_count;
    size_t window_sizes[SOLVER_CODEKD_TEST_WINDOW_CAPACITY];
    size_t retired_candidates;
    size_t candidate_count;
    size_t candidate_cursor;
    size_t retire_descriptor;
    unsigned long long delivery_windows;
} solver_codekd_test_window_result_t;

typedef struct solver_codekd_test_fallback_result {
    int submit_status;
    int results_ready;
    int quad_compute_ready;
    int quad_delivery_disabled;
    int star_delivery_disabled;
    int has_delivery_ticket;
} solver_codekd_test_fallback_result_t;

int solver_codekd_test_run_candidate_windows(
    solver_codekd_test_window_result_t* result);
int solver_codekd_test_run_nonresident_fallback(
    solver_codekd_test_fallback_result_t* result);
int solver_test_candidate_rolling_windows(void);
int solver_test_candidate_nonresident_zero_submit_falls_back(void);

#endif
