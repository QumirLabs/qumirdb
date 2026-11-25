#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QDB_BIN="${QDB_BIN:-"$(cd "$SCRIPT_DIR/../.." && pwd)/build/bin/qdb"}"
SQL_DIR="$SCRIPT_DIR/queries/sql"
OUT_DIR="${OUT_DIR:-./results}"
QDB_ARGS="${QDB_ARGS:-}"
ALLOW_UNSUPPORTED="${ALLOW_UNSUPPORTED:-0}"

usage() {
    echo "Usage: $0 <data_dir> [query_filter]"
    echo "  data_dir      — directory containing hits.parquet, or its parent containing pq1/"
    echo "  query_filter  — optional comma-separated query ids (e.g. 0,7,42 or q00,q07,q42)"
    echo "Env:"
    echo "  QDB_ARGS          — extra arguments passed to qdb"
    echo "  QDB_BIN           — qdb executable (default: build/bin/qdb)"
    echo "  OUT_DIR           — output directory (default: ./results)"
    echo "  ALLOW_UNSUPPORTED — set to 1 to run queries currently known to be unsupported"
    exit 1
}

[[ $# -lt 1 ]] && usage
DATA_ROOT="$(cd "$1" && pwd)"
QUERY_FILTER="${2:-}"
read -r -a EXTRA_QDB_ARGS <<< "$QDB_ARGS"

if [[ -f "$DATA_ROOT/hits.parquet" ]]; then
    DATA_DIR="$DATA_ROOT"
elif [[ -f "$DATA_ROOT/pq1/hits.parquet" ]]; then
    DATA_DIR="$DATA_ROOT/pq1"
else
    echo "ClickBench dataset not found under $DATA_ROOT (expected hits.parquet or pq1/hits.parquet)" >&2
    exit 1
fi

if [[ ! -x "$QDB_BIN" ]]; then
    echo "qdb binary not found or not executable: $QDB_BIN" >&2
    exit 1
fi

query_enabled() {
    local query_id="${1#q}"
    [[ -z "$QUERY_FILTER" ]] && return 0
    local filter requested
    IFS=',' read -r -a filter <<< "$QUERY_FILTER"
    for requested in "${filter[@]}"; do
        requested="${requested#q}"
        if [[ "$requested" =~ ^0*[0-9]+$ && "$query_id" =~ ^0*[0-9]+$ ]]; then
            [[ "$((10#$requested))" -eq "$((10#$query_id))" ]] && return 0
        fi
    done
    return 1
}

unsupported_reason() {
    case "$1" in
        *) return 1 ;;
    esac
}

# Result rows and diagnostics are kept separately. Echoes
# "<exec> <cpu> <plan> <kernel-build> <jit> <return-code>" on stdout.
run_query() {
    local label="$1"; shift
    local out_file err_file rc seconds cpu plan build jit
    out_file="$(mktemp "$tmpdir/qdb_output.XXXXXX")"
    err_file="$(mktemp "$tmpdir/qdb_stderr.XXXXXX")"
    set +e
    "$QDB_BIN" --timing "$@" > "$out_file" 2> "$err_file"
    rc=$?
    set -e

    printf '\n=== %s (rc=%s) ===\n' "$label" "$rc" >> "$RESULT_FILE"
    cat "$out_file" >> "$RESULT_FILE"
    printf '\n=== %s (rc=%s) ===\n' "$label" "$rc" >> "$LOG_FILE"
    cat "$err_file" >> "$LOG_FILE"
    printf '\n' >> "$LOG_FILE"

    seconds=$(
        sed -nE \
            -e 's/.*Returned [0-9]+ rows in ([0-9]+([.][0-9]+)?) seconds.*/\1/p' \
            -e 's/.*Processed [0-9]+ rowsets in ([0-9]+([.][0-9]+)?).*/\1/p' \
            "$err_file" | tail -n 1
    )
    cpu=$(sed -nE 's/.*Cpu: ([0-9]+([.][0-9]+)?) seconds.*/\1/p' "$err_file" | tail -n 1)
    plan=$(sed -nE 's/.*Planning: ([0-9]+([.][0-9]+)?) seconds.*/\1/p' "$err_file" | tail -n 1)
    build=$(sed -nE 's/.*KernelBuild: ([0-9]+([.][0-9]+)?) seconds.*/\1/p' "$err_file" | tail -n 1)
    jit=$(sed -nE 's/.*Jit(LLVM|Tpde): ([0-9]+([.][0-9]+)?) seconds.*/\2/p' "$err_file" | tail -n 1)
    echo "${seconds:-0} ${cpu:-0} ${plan:-0} ${build:-0} ${jit:-0} $rc"
}

mkdir -p "$OUT_DIR"
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

LOG_FILE="$OUT_DIR/clickbench.log"
RESULT_FILE="$OUT_DIR/clickbench.results"
: > "$LOG_FILE"
: > "$RESULT_FILE"

echo "[qdb] clickbench parquet=$DATA_DIR" | tee -a "$LOG_FILE" >&2
printf "%-8s  %-9s  %-9s  %-9s  %-9s  %-9s  %s\n" "Query" "Plan(s)" "KBuild(s)" "JIT(s)" "Exec(s)" "Cpu(s)" "Status"
printf "%-8s  %-9s  %-9s  %-9s  %-9s  %-9s  %s\n" "------" "-------" "---------" "-------" "-------" "------" "------"

total_s="0"
cpu_s="0"
plan_s="0"
build_s="0"
jit_s="0"
failed=0
skipped=0

for query in "$SQL_DIR"/q*.sql; do
    [[ -f "$query" ]] || continue
    base="$(basename "$query" .sql)"
    query_id="${base#q}"
    query_enabled "$query_id" || continue

    if reason="$(unsupported_reason "$query_id")" && [[ "$ALLOW_UNSUPPORTED" != "1" ]]; then
        printf "%-8s  %-9s  %-9s  %-9s  %-9s  %-9s  SKIPPED (%s)\n" "$base" "-" "-" "-" "-" "-" "$reason"
        echo "[qdb] $base SKIPPED ($reason)" >> "$LOG_FILE"
        (( skipped++ )) || true
        continue
    fi

    read -r seconds cpu plan build jit rc <<< "$(run_query "$base" \
        ${EXTRA_QDB_ARGS[@]+"${EXTRA_QDB_ARGS[@]}"} --sql -i "$query" --data "$DATA_DIR")"

    total_s=$(python3 -c "print(round($total_s + $seconds, 3))")
    cpu_s=$(python3 -c "print(round($cpu_s + $cpu, 3))")
    plan_s=$(python3 -c "print(round($plan_s + $plan, 3))")
    build_s=$(python3 -c "print(round($build_s + $build, 3))")
    jit_s=$(python3 -c "print(round($jit_s + $jit, 3))")

    if [[ $rc -eq 0 ]]; then status="OK"; else status="FAILED"; (( failed++ )) || true; fi
    printf "%-8s  %-9s  %-9s  %-9s  %-9s  %-9s  %s\n" "$base" "$plan" "$build" "$jit" "$seconds" "$cpu" "$status"
    echo "[qdb] $base plan=${plan}s kbuild=${build}s jit=${jit}s exec=${seconds}s cpu=${cpu}s $status" >> "$LOG_FILE"
done

printf "Total (processing): %ss,  Cpu: %ss,  Failed: %s,  Skipped: %s\n" "$total_s" "$cpu_s" "$failed" "$skipped"
printf "Compile (excluded from processing): plan=%ss  kbuild=%ss  jit=%ss\n" "$plan_s" "$build_s" "$jit_s"
echo "[qdb] log: $LOG_FILE  results: $RESULT_FILE" >&2
