#!/usr/bin/env bash
# Launches every node in a cluster config locally, tails their output,
# and shuts them all down on Ctrl-C.
#
#   ./scripts/cluster_up.sh [config] [build-dir]
set -euo pipefail

CONFIG="${1:-scripts/cluster.conf}"
BUILD="${2:-build}"
NODE_BIN="$BUILD/node"

if [ ! -x "$NODE_BIN" ]; then
    echo "no node binary at $NODE_BIN (run: make build)" >&2
    exit 1
fi

IDS=$(grep -E '^\s*node\s+' "$CONFIG" | awk '{print $2}')
if [ -z "$IDS" ]; then
    echo "no nodes found in $CONFIG" >&2
    exit 1
fi

PIDS=()
cleanup() {
    echo
    echo "stopping cluster..."
    for pid in "${PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

for id in $IDS; do
    echo "starting node $id"
    "$NODE_BIN" --id "$id" --config "$CONFIG" &
    PIDS+=($!)
    # Stagger slightly so the startup logs stay readable. Nothing about
    # correctness depends on it: a node that starts first simply retries
    # its outbound connections until its peers come up.
    sleep 0.2
done

echo "cluster up with $(echo "$IDS" | wc -w) nodes. Ctrl-C to stop."
wait
