/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */

#ifndef ASTROMETRY_ENGINE_PRIVATE_H
#define ASTROMETRY_ENGINE_PRIVATE_H

#include "astrometry/engine.h"
#include "astrometry/onefield.h"

double engine_job_imagew(job_t* job);
double engine_job_imageh(job_t* job);

int engine_resolve_index_shard_workers(engine_t* engine, job_t* job);

#endif
