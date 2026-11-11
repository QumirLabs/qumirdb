#!/usr/bin/env bash
# Start or stop a local qdbd cluster: one process per node, all nodes know each other.
#   cluster.sh start <nodes> <threads>    cluster.sh stop
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QDBD_BIN="${QDBD_BIN:-$ROOT/build/bin/qdbd}"
DATA_DIR="${DATA_DIR:?DATA_DIR must point to the directory with <table>.parquet files}"
CLUSTER_DIR="${CLUSTER_DIR:-./cluster}"
NODE_PORT_BASE="${NODE_PORT_BASE:-7100}"   # node i listens on NODE_PORT_BASE + i
HTTP_PORT_BASE="${HTTP_PORT_BASE:-8100}"   # node i serves HTTP on HTTP_PORT_BASE + i
READY_TIMEOUT="${READY_TIMEOUT:-60}"
# Space-separated host of every node, in node-id order (ids start at 1); default: all on this machine.
# Only nodes on 127.0.0.1/localhost are started by this script.
HOSTS="${HOSTS:-}"

stop() {
    local pidfile pid
    for pidfile in "$CLUSTER_DIR"/node*.pid; do
        [[ -f "$pidfile" ]] || continue
        pid="$(cat "$pidfile")"
        kill "$pid" 2>/dev/null || true
        for _ in $(seq 1 50); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.1
        done
        kill -9 "$pid" 2>/dev/null || true
        rm -f "$pidfile"
    done
}

start() {
    local nodes="$1" threads="$2"
    local hosts=() node_args=() i
    if [[ -n "$HOSTS" ]]; then
        read -r -a hosts <<< "$HOSTS"
        [[ ${#hosts[@]} -eq $nodes ]] || { echo "HOSTS has ${#hosts[@]} entries, expected $nodes" >&2; return 1; }
    else
        for i in $(seq 1 "$nodes"); do hosts+=(127.0.0.1); done
    fi
    for i in $(seq 1 "$nodes"); do
        node_args+=(--node "${hosts[i-1]}:$((NODE_PORT_BASE + i)):$i")
    done

    mkdir -p "$CLUSTER_DIR"
    stop
    for i in $(seq 1 "$nodes"); do
        case "${hosts[i-1]}" in 127.0.0.1|localhost) ;; *) continue;; esac
        "$QDBD_BIN" --node-id "$i" --leader-id 1 --http-port "$((HTTP_PORT_BASE + i))" \
            --data "$DATA_DIR" "--scheduler:workers=$threads" "${node_args[@]}" \
            > "$CLUSTER_DIR/node$i.log" 2>&1 &
        echo $! > "$CLUSTER_DIR/node$i.pid"
    done

    # Any HTTP reply (even 404) means the node listens.
    for i in $(seq 1 "$nodes"); do
        [[ -f "$CLUSTER_DIR/node$i.pid" ]] || continue
        local deadline=$((SECONDS + READY_TIMEOUT))
        until curl -s -o /dev/null "http://${hosts[i-1]}:$((HTTP_PORT_BASE + i))/"; do
            if ! kill -0 "$(cat "$CLUSTER_DIR/node$i.pid")" 2>/dev/null || (( SECONDS >= deadline )); then
                echo "node $i is not ready, see $CLUSTER_DIR/node$i.log" >&2
                return 1
            fi
            sleep 0.2
        done
    done
    echo "cluster: $nodes node(s) x $threads thread(s), logs in $CLUSTER_DIR" >&2
}

case "${1:-}" in
    start) [[ $# -eq 3 ]] || { echo "Usage: $0 start <nodes> <threads>" >&2; exit 1; }
           start "$2" "$3" || { stop; exit 1; } ;;
    stop) stop ;;
    *) echo "Usage: $0 start <nodes> <threads> | stop (env: DATA_DIR, QDBD_BIN, CLUSTER_DIR, HOSTS)" >&2; exit 1 ;;
esac
