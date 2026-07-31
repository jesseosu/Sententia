#!/usr/bin/env bash
# The Phase 3 demo: a primary replicates an order file to a backup, and
# both end at the same state checksum, having never shipped a book.
set -uo pipefail

NODE="${1:?usage: replication_demo.sh <node-binary> <orders> [mode]}"
ORDERS="${2:?}"
MODE="${3:-sync}"

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

# Fixed ports collide when two runs overlap: a Release ctest and a Debug
# ctest on the same machine, or a re-run starting before the previous
# run's processes have exited. Derive a port base from the PID so
# concurrent runs cannot land on the same ports. Unit tests avoid this
# entirely by binding port 0 and letting the kernel choose, but the demo
# needs a config file with real port numbers in it.
PORT_BASE=$(( 20000 + (($$ * 7) % 30000) ))
CONFIG="$OUT/cluster.conf"
{
    echo "node 1 127.0.0.1 $PORT_BASE"
    echo "node 2 127.0.0.1 $((PORT_BASE + 1))"
} > "$CONFIG"

# Backup first, so the primary's first dial succeeds.
"$NODE" --id 2 --config "$CONFIG" --role backup --mode "$MODE" \
        --exit-on-complete --max-cycles 20000 --quiet > "$OUT/backup.log" 2>&1 &
P2=$!
sleep 0.3
"$NODE" --id 1 --config "$CONFIG" --role primary --mode "$MODE" --orders "$ORDERS" \
        --exit-on-complete --max-cycles 20000 --quiet > "$OUT/primary.log" 2>&1 &
P1=$!

wait $P1; wait $P2

fail() {
    echo "FAIL: $1"
    echo "--- primary ---"; cat "$OUT/primary.log"
    echo "--- backup ---";  cat "$OUT/backup.log"
    exit 1
}

PRIMARY_SUM=$(grep 'state_checksum=' "$OUT/primary.log" | tail -1 | cut -d= -f2)
BACKUP_SUM=$(grep  'state_checksum=' "$OUT/backup.log"  | tail -1 | cut -d= -f2)
PRIMARY_APPLIED=$(grep 'last_applied=' "$OUT/primary.log" | tail -1 | cut -d= -f2)
BACKUP_APPLIED=$(grep  'last_applied=' "$OUT/backup.log"  | tail -1 | cut -d= -f2)

[ -n "$PRIMARY_SUM" ] || fail "primary produced no checksum"
[ -n "$BACKUP_SUM" ]  || fail "backup produced no checksum"
[ "$PRIMARY_APPLIED" -gt 0 ] 2>/dev/null || fail "primary applied nothing"

if [ "$PRIMARY_SUM" != "$BACKUP_SUM" ]; then
    fail "state checksums differ: primary=$PRIMARY_SUM backup=$BACKUP_SUM"
fi
if [ "$PRIMARY_APPLIED" != "$BACKUP_APPLIED" ]; then
    fail "applied counts differ: primary=$PRIMARY_APPLIED backup=$BACKUP_APPLIED"
fi
grep -q 'checksum_mismatches=0' "$OUT/primary.log" || fail "primary saw divergence"

echo "replication demo OK (mode=$MODE)"
echo "  commands applied on both nodes: $PRIMARY_APPLIED"
echo "  state checksum on both nodes:   $PRIMARY_SUM"
