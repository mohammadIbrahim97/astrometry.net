#!/usr/bin/env bash
# This file is part of the Astrometry.net suite.
# Licensed under a 3-clause BSD style license - see LICENSE

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

solver_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)" ||
    die "cannot resolve the solver directory"
source_root="$(cd "$solver_dir/.." && pwd)" ||
    die "cannot resolve the source root"
field="${1:-$source_root/demo/apod4.xyls}"
winner_index="${2:-$solver_dir/index-9918.fits}"
nonwinner_index="${3:-$source_root/demo/index-4119.fits}"
work_dir="${SOLVER_TEST_OUTPUT_DIR:-}"
temporary_output=0

if [[ -z "$work_dir" ]]; then
    work_dir="$(mktemp -d)" ||
        die "cannot create a temporary validation directory"
    temporary_output=1
elif [[ "$work_dir" != /* ]]; then
    die "SOLVER_TEST_OUTPUT_DIR must be absolute"
elif [[ -e "$work_dir" && -n "$(find "$work_dir" -mindepth 1 -print -quit)" ]]; then
    die "SOLVER_TEST_OUTPUT_DIR must be empty"
else
    mkdir -p "$work_dir" || die "cannot create the output directory"
fi

cleanup() {
    local status=$?

    if [[ "$temporary_output" -eq 1 ]]; then
        rm -rf -- "$work_dir" || status=1
    fi
    trap - EXIT
    exit "$status"
}
trap cleanup EXIT

build_tests() {
    make -C "$source_root/util" -j2 test_fitsbin_payload_io \
        >"$work_dir/build-util.log" 2>&1 || {
        tail -80 "$work_dir/build-util.log" >&2
        die "payload test did not build"
    }
    make -C "$solver_dir" -j2 \
        test-solver test-solver-2 test-index-shard-config \
        test-index-shard-staged test-index-shard-pool-lifecycle \
        test-engine-passes \
        test-solver-parallel-integration \
        test-solver-streaming-integration \
        test-solver-allocation-failure \
        test-solver-geometry-fallback \
        test-verify-theta-tail \
        >"$work_dir/build-solver.log" 2>&1 || {
        tail -80 "$work_dir/build-solver.log" >&2
        die "solver tests did not build"
    }
}

run_unit() {
    local name="$1"
    shift

    "$@" >"$work_dir/$name.log" 2>&1 || {
        tail -80 "$work_dir/$name.log" >&2
        die "$name failed"
    }
}

run_case() {
    local name="$1"
    local binary="$2"
    local workers="$3"
    local first_object="$4"
    local last_object="$5"
    local mode="$6"
    local status
    shift 6

    timeout 45 "$solver_dir/$binary" \
        "$field" "$workers" "$first_object" "$last_object" \
        "$work_dir/$name.wcs" "$mode" "$@" \
        >"$work_dir/$name.log" 2>&1
    status=$?
    if [[ "$status" -ne 0 ]]; then
        tail -80 "$work_dir/$name.log" >&2
        die "$name failed with status $status"
    fi
}

run_cancel_case() {
    local name="cancel"
    local log="$work_dir/$name.log"
    local request="$work_dir/$name.request"
    local run_pid
    local status
    local polls=0
    local triggered=0

    timeout 45 env SOLVER_TEST_CANCEL_FILE="$request" \
        "$solver_dir/test-solver-parallel-integration" \
        "$field" 4 1 100 "$work_dir/$name.wcs" cancel "$winner_index" \
        >"$log" 2>&1 &
    run_pid=$!
    while [[ "$polls" -lt 2000 ]]; do
        if grep -qE '\[solver-ab-phase\].* mode=assisted ' \
                "$log" 2>/dev/null; then
            : >"$request" || die "cannot create cancellation request"
            triggered=1
            break
        fi
        if ! kill -0 "$run_pid" 2>/dev/null; then
            break
        fi
        polls=$((polls + 1))
        sleep 0.01 || die "cancellation polling failed"
    done
    wait "$run_pid"
    status=$?
    if [[ "$triggered" -ne 1 || "$status" -ne 0 ]]; then
        tail -80 "$log" >&2
        die "cancellation case failed"
    fi
}

science_key() {
    awk '
        /\[solver\] phase-profile|\[index-shard\] solver-pass/ {
            profile = ""
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^(codekd_calls|codekd_hits|resolve_calls|verify_calls|hypothesis_order|kd_result_order|candidate_order)=/) {
                    profile = profile $i " "
                }
            }
        }
        /^SOLVER_TEST_RESULT / {
            result = $0
            sub(/workers=[0-9]+ /, "", result)
        }
        /^SOLVER_TEST_WCS / {
            wcs = $0
            sub(/workers=[0-9]+ /, "", wcs)
        }
        END {
            print profile
            print result
            print wcs
        }
    ' "$1"
}

science_sequence_key() {
    awk '
        /\[solver\] phase-profile|\[index-shard\] solver-pass/ {
            profile = ""
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^(codekd_calls|codekd_hits|resolve_calls|verify_calls|hypothesis_order|kd_result_order|candidate_order)=/) {
                    profile = profile $i " "
                }
            }
            print profile
        }
        /^SOLVER_TEST_RESULT / {
            line = $0
            sub(/workers=[0-9]+ /, "", line)
            print line
        }
        /^SOLVER_TEST_WCS / {
            line = $0
            sub(/workers=[0-9]+ /, "", line)
            print line
        }
    ' "$1"
}

result_key() {
    awk '
        /^SOLVER_TEST_RESULT / || /^SOLVER_TEST_WCS / {
            line = $0
            sub(/workers=[0-9]+ /, "", line)
            print line
        }
    ' "$1"
}

assert_same() {
    local label="$1"
    local expected="$2"
    local actual="$3"

    if [[ "$expected" != "$actual" ]]; then
        printf '%s differs\nexpected:\n%s\nactual:\n%s\n' \
            "$label" "$expected" "$actual" >&2
        exit 1
    fi
}

assert_log() {
    local label="$1"
    local pattern="$2"
    local file="$3"

    grep -qE "$pattern" "$file" || {
        tail -80 "$file" >&2
        die "$label is missing"
    }
}

compare_pair() {
    local name="$1"
    local first_object="$2"
    local last_object="$3"
    local mode="$4"
    shift 4

    run_case "${name}_w1" test-solver-parallel-integration \
        1 "$first_object" "$last_object" "$mode" "$@"
    run_case "${name}_w4" test-solver-parallel-integration \
        4 "$first_object" "$last_object" "$mode" "$@"
    assert_same "$name science" \
        "$(science_key "$work_dir/${name}_w1.log")" \
        "$(science_key "$work_dir/${name}_w4.log")"
}

build_tests
run_unit payload "$source_root/util/test_fitsbin_payload_io"
run_unit solver "$solver_dir/test-solver"
run_unit solver-permutations "$solver_dir/test-solver-2"
run_unit shard-config "$solver_dir/test-index-shard-config"
run_unit shard-lifecycle "$solver_dir/test-index-shard-staged"
run_unit shard-pool-lifecycle "$solver_dir/test-index-shard-pool-lifecycle"
run_unit engine-passes "$solver_dir/test-engine-passes"
run_unit verify-theta-tail "$solver_dir/test-verify-theta-tail"

run_unit canonical-order \
    "$solver_dir/test-solver-parallel-integration" --canonical-index-order

# Exercise the production onefield hook with enough distinct names to span
# many string-list blocks. Every worker lookup must use the pass snapshot;
# the mutable master list is never traversed after generation publication.
snapshot_indexes=()
for snapshot_order in $(seq 0 348); do
    snapshot_path="$work_dir/index-snapshot-$(printf '%03d' "$snapshot_order").fits"
    ln -s "$winner_index" "$snapshot_path" ||
        die "cannot create immutable-index snapshot fixture"
    snapshot_indexes+=("$snapshot_path")
done
run_case immutable-index-names test-solver-parallel-integration \
    4 1 1 probe "${snapshot_indexes[@]}"
assert_log "immutable index-name candidate count" \
    'candidates=349 ' "$work_dir/immutable-index-names.log"
for snapshot_order in $(seq 0 348); do
    snapshot_path="$work_dir/index-snapshot-$(printf '%03d' "$snapshot_order").fits"
    assert_log "immutable index-name order $snapshot_order" \
        "load index_order=$snapshot_order index=$snapshot_path" \
        "$work_dir/immutable-index-names.log"
done

compare_pair exhaustive 1 20 exhaustive "$winner_index"
SOLVER_TEST_RDLS_TAGALONG_GUARD=1 \
run_case tagalong-guard_w1 test-solver-parallel-integration \
    1 1 20 exhaustive "$winner_index"
SOLVER_TEST_RDLS_TAGALONG_GUARD=1 \
run_case tagalong-guard_w4 test-solver-parallel-integration \
    4 1 20 exhaustive "$winner_index"
assert_same "tag-along serial guard science" \
    "$(science_key "$work_dir/tagalong-guard_w1.log")" \
    "$(science_key "$work_dir/tagalong-guard_w4.log")"
assert_log "tag-along serial guard" \
    'mode=serial-rdls-tagalong ' "$work_dir/tagalong-guard_w4.log"
run_case later-winner_w1 test-solver-parallel-integration \
    1 1 20 winner "$nonwinner_index" "$winner_index"
run_case later-winner_w4 test-solver-parallel-integration \
    4 1 20 winner "$nonwinner_index" "$winner_index"
assert_same "later winner result" \
    "$(result_key "$work_dir/later-winner_w1.log")" \
    "$(result_key "$work_dir/later-winner_w4.log")"
assert_log "ordered later winner" \
    'committed-solution index_order=1 ' \
    "$work_dir/later-winner_w4.log"

SOLVER_TEST_SECOND_FIRST_OBJECT=11 \
SOLVER_TEST_SECOND_LAST_OBJECT=20 \
run_case multipass_w1 test-solver-parallel-integration \
    1 1 10 multipass "$winner_index"
SOLVER_TEST_SECOND_FIRST_OBJECT=11 \
SOLVER_TEST_SECOND_LAST_OBJECT=20 \
run_case multipass_w4 test-solver-parallel-integration \
    4 1 10 multipass "$winner_index"
assert_same "multipass science" \
    "$(science_sequence_key "$work_dir/multipass_w1.log")" \
    "$(science_sequence_key "$work_dir/multipass_w4.log")"

run_case allocation-fallback test-solver-allocation-failure \
    4 1 20 exhaustive "$winner_index"
assert_same "allocation fallback science" \
    "$(science_key "$work_dir/exhaustive_w1.log")" \
    "$(science_key "$work_dir/allocation-fallback.log")"
assert_log "allocation fallback" \
    'alloc_failures=1 ' "$work_dir/allocation-fallback.log"

run_case geometry-reference test-solver-parallel-integration \
    1 5 15 probe "$winner_index"
run_case geometry-fallback test-solver-geometry-fallback \
    4 5 15 probe "$winner_index"
assert_same "geometry fallback science" \
    "$(science_key "$work_dir/geometry-reference.log")" \
    "$(science_key "$work_dir/geometry-fallback.log")"
assert_log "geometry fallback" \
    '\[solver-geometry\] mode=native reason=budget' \
    "$work_dir/geometry-fallback.log"

run_case streaming test-solver-streaming-integration \
    4 1 20 exhaustive "$winner_index"
assert_same "streaming science" \
    "$(science_key "$work_dir/exhaustive_w1.log")" \
    "$(science_key "$work_dir/streaming.log")"
assert_log "streaming delivery" \
    '(\[solver\] page-pipeline|\[index-shard\] solver-pass).*windows=([2-9]|[1-9][0-9]+)' \
    "$work_dir/streaming.log"

run_cancel_case
assert_log "cancellation" \
    '^SOLVER_TEST_RESULT .*cancelled=1 ' "$work_dir/cancel.log"

SOLVER_TEST_TOTAL_WALL_LIMIT=0.000001 \
run_case limit test-solver-parallel-integration \
    4 1 20 limit "$winner_index"
assert_log "wall limit" \
    '^SOLVER_TEST_RESULT .*cancelled=0 wall_limit=1 cpu_limit=0 failed=0 ' \
    "$work_dir/limit.log"
if [[ -s "$work_dir/limit.wcs" ]]; then
    die "wall-limit case published WCS output"
fi

SOLVER_TEST_TOTAL_CPU_LIMIT=0.000001 \
run_case cpu-limit test-solver-parallel-integration \
    4 1 20 limit "$winner_index"
assert_log "CPU limit" \
    '^SOLVER_TEST_RESULT .*cancelled=0 wall_limit=0 cpu_limit=1 failed=0 ' \
    "$work_dir/cpu-limit.log"
if [[ -s "$work_dir/cpu-limit.wcs" ]]; then
    die "CPU-limit case published WCS output"
fi

if grep -qE 'failed=1|serial-precommit-retry' "$work_dir"/*.log; then
    grep -nE 'failed=1|serial-precommit-retry' "$work_dir"/*.log >&2
    die "an integration log contains a failed solver state"
fi

printf 'solver parallel integration checks passed: output=%s\n' \
    "$([[ "$temporary_output" -eq 1 ]] && printf transient || printf '%s' "$work_dir")"
