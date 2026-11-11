#!/usr/bin/env bash
# TPC-H SF10 on a local qdbd cluster, results checked against a single-process qdb run.
#   run_tpch_cluster.sh <1node|4node> <tpch_base_dir> [query_filter]
# 1node = 1 node x 16 threads, 4node = 4 nodes x 4 threads. The cluster is stopped on exit.
# Env: QDB_BIN, QDBD_BIN, OUT_DIR, HOSTS (see cluster.sh);
#      EXPLAIN=1 sends "EXPLAIN <query>" and skips the result check (the cluster cannot execute yet).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
QDB_BIN="${QDB_BIN:-$ROOT/build/bin/qdb}"
OUT_DIR="$(mkdir -p "${OUT_DIR:-./results-cluster}" && cd "${OUT_DIR:-./results-cluster}" && pwd)"
EXPLAIN="${EXPLAIN:-0}"
HTTP_PORT_BASE="${HTTP_PORT_BASE:-8100}"

[[ $# -ge 2 ]] || { echo "Usage: $0 <1node|4node> <tpch_base_dir> [query_filter]" >&2; exit 1; }
case "$1" in
    1node) NODES=1; THREADS=16 ;;
    4node) NODES=4; THREADS=4 ;;
    *) echo "Unknown preset: $1" >&2; exit 1 ;;
esac
BASE_DIR="$(cd "$2" && pwd)"
QUERY_FILTER="${3:-}"
export DATA_DIR="$BASE_DIR/pq10" QDB_BIN HTTP_PORT_BASE CLUSTER_DIR="$OUT_DIR/cluster"
export QDBD_BIN="${QDBD_BIN:-$ROOT/build/bin/qdbd}"
FIRST_HOST="$(read -r -a h <<< "${HOSTS:-127.0.0.1}"; echo "${h[0]}")"

# Reference: the existing runner renders the SQL with the sf10 params and runs one qdb process.
if [[ "$EXPLAIN" != 1 ]]; then
    MODE=sql OUT_DIR="$OUT_DIR/local" QDB_ARGS="--format <escape>csv" bash "$ROOT/benchmark/tpch/run_tpch.sh" "$BASE_DIR" 10 "$QUERY_FILTER"
else
    MODE=sql OUT_DIR="$OUT_DIR/local" RENDER_ONLY=1 bash "$ROOT/benchmark/tpch/run_tpch.sh" "$BASE_DIR" 10 "$QUERY_FILTER" > /dev/null
fi

trap 'bash "$HERE/cluster.sh" stop' EXIT
bash "$HERE/cluster.sh" start "$NODES" "$THREADS"

TIMES="$OUT_DIR/times.tsv"
: > "$TIMES"
failed=0
printf "%-6s %-10s %s\n" Query Wall\(s\) Status
for q in $(seq 1 22); do
    case ",$QUERY_FILTER," in ,,|*,"$q",*) ;; *) continue;; esac
    sql="$OUT_DIR/local/queries/q${q}_sf10.sql"
    out="$OUT_DIR/q${q}.csv"
    body="$OUT_DIR/q${q}.request.sql"
    if [[ "$EXPLAIN" == 1 ]]; then { echo "EXPLAIN"; cat "$sql"; } > "$body"; else cp "$sql" "$body"; fi
    rm -f "$out"
    code_time="$(curl -s -o "$out" -w '%{http_code} %{time_total}' --max-time 3600 \
        --data-binary "@$body" "http://$FIRST_HOST:$((HTTP_PORT_BASE + 1))/api/run")" || code_time="000 0"
    read -r code wall <<< "$code_time"
    status=OK
    if [[ "$code" != 200 || ! -f "$out" ]]; then
        status="ERROR(http $code): $( [[ -f "$out" ]] && head -c 200 "$out" | tr '\n' ' ' || true)"
    elif [[ "$EXPLAIN" != 1 ]]; then
        reference="$OUT_DIR/local/queries/Q${q}_sf10.stdout"
        verdict="$(python3 -I -c '
import sys
sys.path.insert(0, sys.argv[1])
from compare_results import compare_csv
print(compare_csv(sys.argv[2], sys.argv[3], open(sys.argv[4]).read())["status"])
' "$ROOT/benchmark" "$reference" "$out" "$sql")"
        [[ "$verdict" == equal ]] || status="MISMATCH($verdict)"
    fi
    [[ "$status" == OK ]] || failed=$((failed + 1))
    printf "%s\t%s\t%s\n" "$q" "$wall" "$status" >> "$TIMES"
    printf "%-6s %-10s %s\n" "Q$q" "$wall" "$status"
done
python3 -I -c '
import sys
rows = [l.rstrip("\n").split("\t") for l in open(sys.argv[1])]
print("Total wall: %.3fs" % sum(float(r[1]) for r in rows))
' "$TIMES"
echo "Failed: $failed  (times: $TIMES, node logs: $CLUSTER_DIR)"
[[ $failed -eq 0 ]]
