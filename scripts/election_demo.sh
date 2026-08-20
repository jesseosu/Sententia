#!/usr/bin/env bash
# The Phase 4 demo: three real processes elect a leader, the leader is
# killed, and the survivors elect a replacement without anyone helping.
#
# Asserts the two things that matter: a leader appears, and after the
# kill a DIFFERENT node leads in a STRICTLY HIGHER term. The higher term
# is the point. It is what makes the dead leader harmless if it ever
# comes back.
set -uo pipefail

NODE="${1:?usage: election_demo.sh <node-binary>}"

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"; kill $(jobs -p) 2>/dev/null' EXIT

# PID-derived ports so concurrent runs cannot collide.
PORT_BASE=$(( 20000 + (($$ * 11) % 30000) ))
CONFIG="$OUT/cluster.conf"
for i in 1 2 3; do
    echo "node $i 127.0.0.1 $((PORT_BASE + i))"
done > "$CONFIG"

declare -A PIDS
for i in 1 2 3; do
    "$NODE" --id "$i" --config "$CONFIG" --elect --max-cycles 600 --quiet \
        > "$OUT/n$i.log" 2>&1 &
    PIDS[$i]=$!
done

fail() {
    echo "FAIL: $1"
    for i in 1 2 3; do echo "--- node $i ---"; cat "$OUT/n$i.log"; done
    exit 1
}

# Wait for a first leader, bounded.
LEADER=""
for _ in $(seq 1 100); do
    for i in 1 2 3; do
        if grep -q "ELECTED LEADER" "$OUT/n$i.log" 2>/dev/null; then LEADER=$i; break; fi
    done
    [ -n "$LEADER" ] && break
    sleep 0.1
done
[ -n "$LEADER" ] || fail "no leader was elected"

FIRST_TERM=$(grep -h "ELECTED LEADER" "$OUT/n$LEADER.log" | head -1 | grep -o '[0-9]*$')
echo "node $LEADER elected leader for term $FIRST_TERM"

# Exactly one node claimed leadership of that first term.
CLAIMS=$(grep -h "ELECTED LEADER for term $FIRST_TERM" "$OUT"/n*.log | wc -l)
[ "$CLAIMS" -eq 1 ] || fail "term $FIRST_TERM had $CLAIMS leaders (split brain)"

# Kill the leader outright. No graceful shutdown, no handover.
kill -9 "${PIDS[$LEADER]}" 2>/dev/null
echo "killed node $LEADER"

# A survivor must take over, in a higher term.
NEW_LEADER=""
NEW_TERM=""
for _ in $(seq 1 150); do
    for i in 1 2 3; do
        [ "$i" = "$LEADER" ] && continue
        T=$(grep -h "ELECTED LEADER" "$OUT/n$i.log" 2>/dev/null | tail -1 | grep -o '[0-9]*$')
        if [ -n "$T" ] && [ "$T" -gt "$FIRST_TERM" ]; then
            NEW_LEADER=$i
            NEW_TERM=$T
            break
        fi
    done
    [ -n "$NEW_LEADER" ] && break
    sleep 0.1
done

[ -n "$NEW_LEADER" ] || fail "no new leader after the old one was killed"
[ "$NEW_LEADER" != "$LEADER" ] || fail "the dead node somehow led again"

wait "${PIDS[$i]}" 2>/dev/null
sleep 0.5

# No term anywhere ever had two leaders.
for T in $(grep -h "ELECTED LEADER" "$OUT"/n*.log | grep -o '[0-9]*$' | sort -u); do
    N=$(grep -h "ELECTED LEADER for term $T" "$OUT"/n*.log | wc -l)
    [ "$N" -eq 1 ] || fail "term $T had $N leaders (split brain)"
done

echo "election demo OK"
echo "  first leader:  node $LEADER, term $FIRST_TERM"
echo "  after failure: node $NEW_LEADER, term $NEW_TERM"
echo "  no term ever had two leaders"
