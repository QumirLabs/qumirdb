#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QDB_BIN="${QDB_BIN:-"$(cd "$SCRIPT_DIR/../.." && pwd)/build/bin/qdb"}"
SEXPR_DIR="$SCRIPT_DIR/queries/sexpr"
SQL_DIR="$SCRIPT_DIR/queries/sql"
PARAMS_DIR="$SCRIPT_DIR/params"
OUT_DIR="${OUT_DIR:-./results}"
MODE="${MODE:-sexpr}"
QDB_ARGS="${QDB_ARGS:-}"
QDB_ARGS_FILE="${QDB_ARGS_FILE:-}"
RENDER_ONLY="${RENDER_ONLY:-0}"
FROZEN_QUERY_DIR="${FROZEN_QUERY_DIR:-}"

usage() {
    echo "Usage: $0 <base_dir> [scale] [query_filter]"
    echo "  base_dir      — directory containing pq1/, pq10/, pq100/ subdirs with parquet files"
    echo "  scale         — scale factor number: 1, 10, or 100 (default: all discovered)"
    echo "  query_filter  — optional comma-separated list of query numbers to run (e.g. 1,3,7)"
    echo "Env:"
    echo "  MODE          — sexpr (default, runs queries/sexpr/*.sexp) or sql (runs queries/sql/*.sql via --sql)"
    echo "  QDB_ARGS      — extra arguments passed to qdb, e.g. '--scheduler threaded --scheduler-workers 4'"
    echo "  QDB_ARGS_FILE — optional NUL-separated arguments (preserves spaces; overrides QDB_ARGS)"
    echo "  RENDER_ONLY=1 — save queries under OUT_DIR/queries without executing qdb"
    echo "  FROZEN_QUERY_DIR — execute previously rendered queries from this directory"
    exit 1
}

[[ $# -lt 1 ]] && usage
[[ "$MODE" != "sexpr" && "$MODE" != "sql" ]] && { echo "Unknown MODE: $MODE" >&2; usage; }
BASE_DIR="$(cd "$1" && pwd)"
SCALE_FILTER="${2:-}"
QUERY_FILTER="${3:-}"
read -r -a EXTRA_QDB_ARGS <<< "$QDB_ARGS"
if [[ -n "$QDB_ARGS_FILE" ]]; then
    EXTRA_QDB_ARGS=()
    while IFS= read -r -d '' argument; do
        EXTRA_QDB_ARGS+=("$argument")
    done < "$QDB_ARGS_FILE"
fi

# Discover scale subdirectories (pq<N>)
ALL_SCALES=()
for d in "$BASE_DIR"/pq*/; do
    [[ -d "$d" ]] && ALL_SCALES+=("$(basename "$d")")
done

if [[ ${#ALL_SCALES[@]} -eq 0 ]]; then
    echo "No pq* subdirectories found under $BASE_DIR" >&2
    exit 1
fi

# Filter to requested scale if given
SCALES=()
if [[ -n "$SCALE_FILTER" ]]; then
    target="pq${SCALE_FILTER}"
    for s in "${ALL_SCALES[@]}"; do
        [[ "$s" == "$target" ]] && SCALES+=("$s")
    done
    if [[ ${#SCALES[@]} -eq 0 ]]; then
        echo "Scale pq${SCALE_FILTER} not found under $BASE_DIR" >&2
        exit 1
    fi
else
    SCALES=("${ALL_SCALES[@]}")
fi

query_enabled() {
    local n="$1"
    [[ -z "$QUERY_FILTER" ]] && return 0
    case ",$QUERY_FILTER," in *,"$n",*) return 0;; esac
    return 1
}

# Run qdb with the given args; result rows go to RESULT_FILE, diagnostics to
# LOG_FILE. Echoes "<exec> <cpu> <plan> <kernel-build> <llvm> <return-code>".
run_query() {
    local label="$1"; shift
    python3 "$SCRIPT_DIR/../query_runner.py" \
        "$QDB_BIN" "$label" "$RESULT_FILE" "$LOG_FILE" "$METRICS_FILE" "$@"
}

# Substitute scale params (and, for sexpr, table paths) into a template (stdout).
# sexpr templates use day-int dates (__Q3_DATE__); SQL templates use date-string
# placeholders (__Q3_DATE_S__) — both sets are substituted here.
render() {
    local template="$1" data_dir="$2"
    sed \
        -e "s|__Q1_DELTA__|${Q1_DELTA:-0}|g" \
        -e "s|__Q3_DATE_S__|${Q3_DATE_S:-}|g" \
        -e "s|__Q4_DATE_S__|${Q4_DATE_S:-}|g" \
        -e "s|__Q5_DATE_S__|${Q5_DATE_S:-}|g" \
        -e "s|__Q6_DATE_S__|${Q6_DATE_S:-}|g" \
        -e "s|__Q10_DATE_S__|${Q10_DATE_S:-}|g" \
        -e "s|__Q12_DATE_S__|${Q12_DATE_S:-}|g" \
        -e "s|__Q14_DATE_S__|${Q14_DATE_S:-}|g" \
        -e "s|__Q15_DATE_S__|${Q15_DATE_S:-}|g" \
        -e "s|__LINEITEM__|${data_dir}/lineitem.parquet|g" \
        -e "s|__ORDERS__|${data_dir}/orders.parquet|g" \
        -e "s|__CUSTOMER__|${data_dir}/customer.parquet|g" \
        -e "s|__SUPPLIER__|${data_dir}/supplier.parquet|g" \
        -e "s|__PART__|${data_dir}/part.parquet|g" \
        -e "s|__PARTSUPP__|${data_dir}/partsupp.parquet|g" \
        -e "s|__NATION__|${data_dir}/nation.parquet|g" \
        -e "s|__REGION__|${data_dir}/region.parquet|g" \
        -e "s|__Q1_SHIPDATE_MAX__|${Q1_SHIPDATE_MAX:-0}|g" \
        -e "s|__Q2_PART_SIZE__|${Q2_PART_SIZE:-0}|g" \
        -e "s|__Q2_REGION__|${Q2_REGION:-}|g" \
        -e "s|__Q3_SEGMENT__|${Q3_SEGMENT:-}|g" \
        -e "s|__Q3_DATE__|${Q3_DATE:-0}|g" \
        -e "s|__Q4_DATE_LOW__|${Q4_DATE_LOW:-0}|g" \
        -e "s|__Q4_DATE_HIGH__|${Q4_DATE_HIGH:-0}|g" \
        -e "s|__Q5_DATE_LOW__|${Q5_DATE_LOW:-0}|g" \
        -e "s|__Q5_DATE_HIGH__|${Q5_DATE_HIGH:-0}|g" \
        -e "s|__Q5_REGION__|${Q5_REGION:-}|g" \
        -e "s|__Q6_DATE_LOW__|${Q6_DATE_LOW:-0}|g" \
        -e "s|__Q6_DATE_HIGH__|${Q6_DATE_HIGH:-0}|g" \
        -e "s|__Q6_DISC_LOW__|${Q6_DISC_LOW:-0.0}|g" \
        -e "s|__Q6_DISC_HIGH__|${Q6_DISC_HIGH:-0.0}|g" \
        -e "s|__Q6_QUANTITY__|${Q6_QUANTITY:-0.0}|g" \
        -e "s|__Q7_NATION1__|${Q7_NATION1:-}|g" \
        -e "s|__Q7_NATION2__|${Q7_NATION2:-}|g" \
        -e "s|__Q8_DATE_LOW__|${Q8_DATE_LOW:-0}|g" \
        -e "s|__Q8_DATE_HIGH__|${Q8_DATE_HIGH:-0}|g" \
        -e "s|__Q8_REGION__|${Q8_REGION:-}|g" \
        -e "s|__Q8_PTYPE__|${Q8_PTYPE:-}|g" \
        -e "s|__Q8_NATION__|${Q8_NATION:-}|g" \
        -e "s|__Q10_DATE_LOW__|${Q10_DATE_LOW:-0}|g" \
        -e "s|__Q10_DATE_HIGH__|${Q10_DATE_HIGH:-0}|g" \
        -e "s|__Q11_NATION__|${Q11_NATION:-}|g" \
        -e "s|__Q12_SHIPMODE1__|${Q12_SHIPMODE1:-}|g" \
        -e "s|__Q12_SHIPMODE2__|${Q12_SHIPMODE2:-}|g" \
        -e "s|__Q12_DATE_LOW__|${Q12_DATE_LOW:-0}|g" \
        -e "s|__Q12_DATE_HIGH__|${Q12_DATE_HIGH:-0}|g" \
        -e "s|__Q14_DATE_LOW__|${Q14_DATE_LOW:-0}|g" \
        -e "s|__Q14_DATE_HIGH__|${Q14_DATE_HIGH:-0}|g" \
        -e "s|__Q15_DATE_LOW__|${Q15_DATE_LOW:-0}|g" \
        -e "s|__Q15_DATE_HIGH__|${Q15_DATE_HIGH:-0}|g" \
        -e "s|__Q16_BRAND__|${Q16_BRAND:-}|g" \
        -e "s|__Q17_BRAND__|${Q17_BRAND:-}|g" \
        -e "s|__Q17_CONTAINER__|${Q17_CONTAINER:-}|g" \
        -e "s|__Q20_NATION__|${Q20_NATION:-}|g" \
        -e "s|__Q20_DATE_LOW__|${Q20_DATE_LOW:-0}|g" \
        -e "s|__Q20_DATE_HIGH__|${Q20_DATE_HIGH:-0}|g" \
        -e "s|__Q21_NATION__|${Q21_NATION:-}|g" \
        -e "s|__Q22_CC1__|${Q22_CC1:-}|g" \
        -e "s|__Q22_CC2__|${Q22_CC2:-}|g" \
        -e "s|__Q22_CC3__|${Q22_CC3:-}|g" \
        -e "s|__Q22_CC4__|${Q22_CC4:-}|g" \
        -e "s|__Q22_CC5__|${Q22_CC5:-}|g" \
        -e "s|__Q22_CC6__|${Q22_CC6:-}|g" \
        -e "s|__Q22_CC7__|${Q22_CC7:-}|g" \
        "$template"
}

mkdir -p "$OUT_DIR"

mkdir -p "$OUT_DIR/queries"

for scale in "${SCALES[@]}"; do
    sf_num="${scale#pq}"
    params_file="$PARAMS_DIR/sf${sf_num}.sh"
    data_dir="$BASE_DIR/$scale"

    # Scale params drive placeholder substitution for both modes.
    if [[ ! -f "$params_file" ]]; then
        echo "[qdb] skip $scale — no params file $params_file" >&2
        continue
    fi
    # shellcheck source=/dev/null
    source "$params_file"

    LOG_FILE="$OUT_DIR/tpch_${MODE}_sf${sf_num}.log"
    RESULT_FILE="$OUT_DIR/tpch_${MODE}_sf${sf_num}.results" # query result rows
    : > "$LOG_FILE"
    : > "$RESULT_FILE"
    METRICS_FILE="$OUT_DIR/tpch_${MODE}_sf${sf_num}.jsonl"
    : > "$METRICS_FILE"

    echo "[qdb] mode=$MODE scale=$sf_num parquet=$data_dir" | tee -a "$LOG_FILE" >&2

    printf "%-8s  %-9s  %-9s  %-9s  %-9s  %-9s  %s\n" "Query" "Plan(s)" "KBuild(s)" "LLVM(s)" "Exec(s)" "Cpu(s)" "Status"
    printf "%-8s  %-9s  %-9s  %-9s  %-9s  %-9s  %s\n" "------" "-------" "---------" "-------" "-------" "------" "------"

    total_s="0"
    cpu_s="0"
    plan_s="0"
    build_s="0"
    llvm_s="0"
    failed=0

    for q_num in $(seq 1 22); do
        query_enabled "$q_num" || continue

        if [[ "$MODE" == "sql" ]]; then
            template="$SQL_DIR/q${q_num}.sql"
            extension=sql
        else
            template="$SEXPR_DIR/q${q_num}.sexp"
            extension=sexp
        fi
        [[ -f "$template" ]] || continue
        if [[ -n "$FROZEN_QUERY_DIR" ]]; then
            query="$FROZEN_QUERY_DIR/q${q_num}_sf${sf_num}.$extension"
            [[ -f "$query" ]] || { echo "Missing frozen query: $query" >&2; exit 1; }
        else
            query="$OUT_DIR/queries/q${q_num}_sf${sf_num}.$extension"
            render "$template" "$data_dir" > "$query"
        fi
        if [[ "$RENDER_ONLY" == "1" ]]; then
            printf '%s\n' "$query"
            continue
        fi
        if [[ "$MODE" == "sql" ]]; then
            read -r seconds cpu plan build llvm rc <<< "$(run_query "Q${q_num} sf${sf_num}" ${EXTRA_QDB_ARGS[@]+"${EXTRA_QDB_ARGS[@]}"} --sql -i "$query" --data "$data_dir")"
        else
            read -r seconds cpu plan build llvm rc <<< "$(run_query "Q${q_num} sf${sf_num}" ${EXTRA_QDB_ARGS[@]+"${EXTRA_QDB_ARGS[@]}"} -i "$query")"
        fi

        total_s=$(python3 -c "print(round($total_s + $seconds, 3))")
        cpu_s=$(python3 -c "print(round($cpu_s + $cpu, 3))")
        plan_s=$(python3 -c "print(round($plan_s + $plan, 3))")
        build_s=$(python3 -c "print(round($build_s + $build, 3))")
        llvm_s=$(python3 -c "print(round($llvm_s + $llvm, 3))")

        if [[ $rc -eq 0 ]]; then status="OK"; else status="FAILED"; (( failed++ )) || true; fi
        printf "%-8s  %-9s  %-9s  %-9s  %-9s  %-9s  %s\n" \
            "Q$q_num" "$plan" "$build" "$llvm" "$seconds" "$cpu" "$status"
        echo "[qdb] q${q_num} plan=${plan}s kbuild=${build}s llvm=${llvm}s exec=${seconds}s cpu=${cpu}s ${status}" >> "$LOG_FILE"
    done

    [[ "$RENDER_ONLY" == "1" ]] && continue
    printf "Total (processing): %ss,  Cpu: %ss,  Failed: %s\n" "$total_s" "$cpu_s" "$failed"
    printf "Compile (excluded from processing): plan=%ss  kbuild=%ss  llvm=%ss\n" "$plan_s" "$build_s" "$llvm_s"
    echo "[qdb] log: $LOG_FILE  results: $RESULT_FILE" >&2
done
