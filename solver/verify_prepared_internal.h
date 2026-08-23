/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */

#ifndef VERIFY_PREPARED_INTERNAL_H
#define VERIFY_PREPARED_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "verify_internal.h"

typedef enum verify_prepared_state {
    VERIFY_PREPARED_READY = 0,
    VERIFY_PREPARED_NO_REFERENCE = 1,
    VERIFY_PREPARED_NO_QUAD_REFERENCE = 2,
    VERIFY_PREPARED_NO_ROR_REFERENCE = 3,
    VERIFY_PREPARED_EMPTY_LISTS = 4
} verify_prepared_state_t;

typedef struct verify_index_query verify_index_query_t;
typedef struct verify_prepared_hit verify_prepared_hit_t;

typedef struct verify_prepared_score {
    double logodds;
    double worstlogodds;
    int besti;
    int ibailed;
    int istopped;
    double* allodds;
    int* theta;
    anbool complete;
} verify_prepared_score_t;

struct verify_index_query {
    /* source is borrowed until this query is consumed or destroyed. */
    const startree_t* source;
    double center[3];
    double radius2;
    double* refxyz;
    int* refstarid;
    uint8_t* sweep;
    int nrall;
};

struct verify_prepared_hit {
    /* Index-backed arrays are owned; verify.testxy borrows verify_field data. */
    verify_t verify;
    sip_t wcs;
    double* refxyz;
    double effective_area;
    double distractors;
    double logbail;
    double logaccept;
    double logstoplooking;
    int nrimage;
    verify_prepared_state_t state;
    anbool fake_match;
};

/*
 * These continuations are private to the solver pipeline. Queries borrow the
 * StarKD source. Prepared hits own their index-derived arrays and borrow only
 * the immutable field until ordered owner completion.
 */
int verify_query_hit(const startree_t* skdt,
                     const double center[3],
                     double radius2,
                     verify_index_query_t** query);
size_t verify_index_query_count(const verify_index_query_t* query);
size_t verify_index_query_bytes(const verify_index_query_t* query);
int verify_index_query_sweep_range(const startree_t* skdt,
                                   const verify_index_query_t* query,
                                   size_t index,
                                   const void** data,
                                   size_t* size);
int verify_index_query_capture_sweep(const startree_t* skdt,
                                     verify_index_query_t* query);
void verify_destroy_index_query(verify_index_query_t* query);

int verify_prepare_hit_from_query(const startree_t* skdt,
                                  verify_index_query_t** query,
                                  int index_cutnside,
                                  const MatchObj* mo,
                                  const sip_t* sip,
                                  const verify_field_t* vf,
                                  double verify_pix2,
                                  double distractors,
                                  double fieldW,
                                  double fieldH,
                                  double logratio_tobail,
                                  double logratio_toaccept,
                                  double logratio_tostoplooking,
                                  anbool distance_from_quad_bonus,
                                  anbool fake_match,
                                  verify_prepared_hit_t** prepared);
int verify_score_prepared_hit(const verify_prepared_hit_t* prepared,
                              verify_prepared_score_t* score);
int verify_finish_prepared_hit(verify_prepared_hit_t* prepared,
                               verify_prepared_score_t* score,
                               MatchObj* mo);
size_t verify_prepared_hit_bytes(const verify_prepared_hit_t* prepared);
size_t verify_prepared_score_bytes(
    const verify_prepared_hit_t* prepared);
size_t verify_prepared_hit_peak_bytes(
    const verify_prepared_hit_t* prepared);
void verify_destroy_prepared_score(verify_prepared_score_t* score);
void verify_destroy_prepared_hit(verify_prepared_hit_t* prepared);

#endif
