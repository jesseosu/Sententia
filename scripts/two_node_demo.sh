#!/usr/bin/env bash
# The two-node heartbeat demo, run as a test.
#
# Starts two real node processes, lets them discover each other and
# exchange heartbeats, then asserts on their logs. Both bound their own
# run with --max-cycles and exit on their own, so this cannot hang
# waiting on a process that never dies.
set -uo pipefail

NODE="${1:?usage: two_node_demo.sh <node-binary> <config>}"
CONFIG="${2:?usage: two_node_demo.sh <node-binary> <config>}"

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

"$NODE" --id 1 --config "$CONFIG" --heartbeat-cycles 5 --max-cycles 100 > "$OUT/n1.log" 2>&1 &
P1=$!
"$NODE" --id 2 --config "$CONFIG" --heartbeat-cycles 5 --max-cycles 100 > "$OUT/n2.log" 2>&1 &
P2=$!

rc=0
wait $P1 || rc=1
wait $P2 || rc=1
if [ "$rc" -ne 0 ]; then
    echo "a node exited non-zero"
    cat "$OUT/n1.log" "$OUT/n2.log"
    exit 1
fi

fail() {
    echo "FAIL: $1"
    echo "--- node 1 ---"; cat "$OUT/n1.log"
    echo "--- node 2 ---"; cat "$OUT/n2.log"
    exit 1
}

# Each node saw the other come up, and identified it by its real id.
grep -q "PEER UP 2" "$OUT/n1.log" || fail "node 1 never saw node 2"
grep -q "PEER UP 1" "$OUT/n2.log" || fail "node 2 never saw node 1"

# Heartbeats crossed in both directions.
grep -q "RECV from 2: HEARTBEAT" "$OUT/n1.log" || fail "node 1 received no heartbeat"
grep -q "RECV from 1: HEARTBEAT" "$OUT/n2.log" || fail "node 2 received no heartbeat"

# No framing errors on either side.
grep -q "framing_errors=0" "$OUT/n1.log" || fail "node 1 reported framing errors"
grep -q "framing_errors=0" "$OUT/n2.log" || fail "node 2 reported framing errors"

echo "two-node demo OK"
grep -E "messages_(sent|received)" "$OUT/n1.log" | sed 's/^/  node1 /'
grep -E "messages_(sent|received)" "$OUT/n2.log" | sed 's/^/  node2 /'
