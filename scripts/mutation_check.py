#!/usr/bin/env python3
"""Mutation testing: prove the tests can actually fail.

A passing test suite tells you very little. It tells you the tests agree
with the code, which is also true when both are wrong, and true when a
test is structurally incapable of observing the thing it is named after.

This project has hit that twice. A snapshot test passed under a
deliberate off-by-one because the setup made the bug impossible to
express. A split-brain test needed to be checked against a broken quorum
before it could be believed. Both were caught by hand, once per phase,
by remembering to try. Remembering is not a mechanism.

So: this script breaks the code on purpose, in specific named ways, and
asserts that a specific test NOTICES. A mutation that survives means
either the test is vacuous or the invariant is unguarded. Both are worth
knowing and neither shows up in a green run.

Usage:
    scripts/mutation_check.py                 run every mutation
    scripts/mutation_check.py --list          show the registry
    scripts/mutation_check.py --only ID       run one
    scripts/mutation_check.py --jobs N        build parallelism
"""

import argparse
import dataclasses
import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
BUILD = REPO / "build-mutants"
ASAN = REPO / "build-asan"


@dataclasses.dataclass
class Mutation:
    ident: str
    # Which invariant this attacks, in plain language.
    breaks: str
    path: str
    find: str
    replace: str
    # Tests expected to fail. At least one must, or the mutation survived.
    killers: list
    # Some mutations produce a memory error rather than a wrong answer,
    # and no assertion can see those. Those run against a sanitizer
    # build instead.
    sanitize: bool = False


# Each entry attacks one property the project actually claims. Where a
# historical bug is being re-created, it says so: those are regression
# guards for defects that really shipped.
MUTATIONS = [
    Mutation(
        "engine-time-priority",
        "Time priority: orders at one price fill in arrival order",
        "src/engine.cpp",
        "book_.rest(RestingOrder{cmd.id, cmd.side, cmd.price, residual, ++arrivalCounter_});",
        "book_.rest(RestingOrder{cmd.id, cmd.side, cmd.price, residual, arrivalCounter_});",
        ["test_price_time_priority", "test_invariants"],
    ),
    Mutation(
        "engine-trades-at-the-wrong-price",
        "Execution is at the resting order's price, so price improvement goes to the aggressor",
        "src/engine.cpp",
        "t.price = restingPrice;",
        "t.price = cmd.type == OrderType::Market ? restingPrice : cmd.price;",
        ["replay_sample", "test_matching_basic"],
    ),
    Mutation(
        "engine-price-priority",
        "Price priority: the best bid is the highest, not the lowest",
        "include/sententia/order_book.hpp",
        "using BidLevels = std::map<Price, OrderQueue, std::greater<Price>>;",
        "using BidLevels = std::map<Price, OrderQueue, std::less<Price>>;",
        ["test_order_book", "test_price_time_priority", "test_matching_basic"],
    ),
    Mutation(
        "engine-snapshot-drops-arrival-counter",
        "Snapshots restore time priority, not just the visible book",
        "src/engine.cpp",
        "snap.arrivalCounter = arrivalCounter_;",
        "snap.arrivalCounter = 0;",
        ["test_durability"],
    ),
    Mutation(
        "framing-ignores-payload-cap",
        "An untrusted length is capped before it becomes an allocation",
        "src/net/framing.cpp",
        "if (length > kMaxPayload) {",
        "if (length > 0xFFFFFFFFu) {",
        ["test_framing"],
    ),
    Mutation(
        "framing-ignores-magic",
        "A desynchronised stream is detected rather than parsed",
        "src/net/framing.cpp",
        "if (magic != kMagic) {",
        "if (magic != kMagic && false) {",
        ["test_framing"],
    ),
    Mutation(
        "codec-drops-a-field",
        "Encoder and decoder agree on every field (regression: defect 9)",
        "src/net/message.cpp",
        "if (!r.u64(m.term) || !r.u32(m.nodeId) || !r.u64(m.lastLogSeq) || !r.u8(ok) ||",
        "if (!r.u64(m.term) || !r.u32(m.nodeId) || !r.u8(ok) ||",
        ["test_message_codec"],
    ),
    Mutation(
        "transport-unbounded-queue",
        "A slow peer cannot grow the sender without bound (regression: defect 4)",
        "src/net/transport.cpp",
        "if (peer->writer.overHighWaterMark()) {",
        "if (peer->writer.overHighWaterMark() && false) {",
        ["test_backpressure"],
    ),
    Mutation(
        "replication-resends-everything",
        "Replication does not amplify (regression: defect 5, the use-after-move)",
        "src/replication/replicator.cpp",
        "backup.lastSent = lastInBatch;",
        "backup.lastSent = lastInBatch - lastInBatch;",
        ["test_replication"],
    ),
    Mutation(
        "replication-skips-a-command",
        "A follower applies every command the leader did",
        "src/replication/replicator.cpp",
        "engine_.apply(entry->command, sink);\n        lastApplied_ = entry->seq;",
        "if (entry->seq % 500 != 0) {\n            engine_.apply(entry->command, sink);\n        }\n        lastApplied_ = entry->seq;",
        ["test_replication", "test_replication_chaos"],
    ),
    Mutation(
        "replication-ignores-log-matching",
        "A follower checks the term at prevSeq, not just the sequence",
        "src/replication/replicator.cpp",
        "if (!localPrevTerm.has_value() || localPrevTerm.value() != msg.prevTerm) {",
        "if (!localPrevTerm.has_value()) {",
        ["test_replication", "test_replication_chaos"],
    ),
    Mutation(
        "election-majority-of-one",
        "A leader needs a majority, so two leaders per term are impossible",
        "include/sententia/consensus/election.hpp",
        "std::size_t majority() const noexcept { return clusterSize() / 2 + 1; }",
        "std::size_t majority() const noexcept { return 1; }",
        ["test_election_sim", "test_election"],
    ),
    Mutation(
        "election-votes-many-times",
        "One vote per node per term, which is what forbids split brain",
        "src/consensus/election.cpp",
        "} else if (votedFor_.has_value() && votedFor_.value() != msg.candidateId) {",
        "} else if (false) {",
        ["test_election_sim", "test_election"],
    ),
    Mutation(
        "election-ignores-higher-term",
        "A node seeing a higher term steps down, which defuses a stale leader",
        "src/consensus/election.cpp",
        "if (term <= currentTerm_) {\n        return false;\n    }",
        "if (term <= currentTerm_ || true) {\n        return false;\n    }",
        ["test_election", "test_election_sim"],
    ),
    Mutation(
        "election-ignores-log-restriction",
        "A candidate behind the voter cannot win, so committed entries survive",
        "src/consensus/election.cpp",
        "} else if (msg.lastLogSeq < lastLogSeq_) {",
        "} else if (false) {",
        ["test_election"],
    ),
    Mutation(
        "wal-trusts-bad-checksums",
        "A torn or corrupt record is discarded, not applied",
        "src/storage/wal.cpp",
        "if (crc32(payload, length) != expectedCrc) {",
        "if (crc32(payload, length) != expectedCrc && false) {",
        ["test_durability", "test_recovery"],
    ),
    Mutation(
        "wal-ignores-short-payload",
        "A header with no payload behind it is a torn write, not a record",
        "src/storage/wal.cpp",
        "if (offset + kWalHeaderSize + length > file.size()) {",
        "if (offset + kWalHeaderSize + length > file.size() + 1000000) {",
        ["test_durability"],
        sanitize=True,
    ),
    Mutation(
        "recovery-double-applies-at-boundary",
        "Recovery skips entries the snapshot already covers (the vacuous-test bug)",
        "src/storage/durable_state.cpp",
        "if (entry->seq <= startAfter) {",
        "if (entry->seq < startAfter) {",
        ["test_recovery"],
    ),
    Mutation(
        "stable-store-forgets-the-vote",
        "Term and vote survive a restart, so a node cannot vote twice per term",
        "src/storage/stable_store.cpp",
        "state_.votedFor = static_cast<NodeId>(getU32(buf.data() + 12));",
        "state_.votedFor = 0;",
        ["test_durability", "test_recovery"],
    ),
    Mutation(
        "commit-ignores-majority",
        "The commit point is a majority, not whatever the leader has",
        "src/replication/replicator.cpp",
        "const Sequence candidate = matches[majority() - 1];",
        "const Sequence candidate = matches[0];",
        ["test_replication"],
    ),
    Mutation(
        "commit-ignores-current-term-rule",
        "Raft Figure 8: a leader only commits entries from its own term",
        "src/replication/replicator.cpp",
        "if (!candidateTerm.has_value() || candidateTerm.value() != term_) {\n        return;\n    }",
        "if (!candidateTerm.has_value()) {\n        return;\n    }",
        ["test_replication", "test_replication_chaos"],
    ),
]


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, **kw)


def configure(jobs, needs_asan):
    BUILD.mkdir(exist_ok=True)
    r = run(["cmake", "-S", ".", "-B", str(BUILD), "-DCMAKE_BUILD_TYPE=Release"])
    if r.returncode != 0:
        print(r.stdout, r.stderr)
        sys.exit("configure failed")
    if needs_asan:
        ASAN.mkdir(exist_ok=True)
        r2 = run(["cmake", "-S", ".", "-B", str(ASAN), "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                  "-DSENTENTIA_SANITIZE=ON"])
        if r2.returncode != 0:
            print(r2.stdout, r2.stderr)
            sys.exit("sanitizer configure failed")
        build(jobs, True)
    return build(jobs, False)


def build(jobs, sanitize=False):
    tree = ASAN if sanitize else BUILD
    return run(["cmake", "--build", str(tree), "-j", str(jobs)])


def run_test(name, sanitize=False):
    tree = ASAN if sanitize else BUILD
    binary = tree / name
    if not binary.exists():
        # Not a test binary. Some tests are ctest-only, such as the
        # golden-checksum replay check which is a CMake script. Run it
        # through ctest by name rather than skipping it, because a
        # silently skipped killer is a mutation that survives for no
        # reason anybody would notice.
        probe = subprocess.run(["ctest", "--test-dir", str(tree), "-N", "-R", f"^{name}$"],
                               cwd=REPO, capture_output=True, text=True)
        if f"Total Tests: 0" in probe.stdout or probe.returncode != 0:
            return None
        r = subprocess.run(["ctest", "--test-dir", str(tree), "-R", f"^{name}$"],
                           cwd=REPO, capture_output=True, text=True, timeout=900)
        return r.returncode
    env = None
    if sanitize:
        import os
        env = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    r = subprocess.run([str(binary)], cwd=REPO, capture_output=True, text=True,
                       timeout=900, env=env)
    return r.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--only")
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args()

    if args.list:
        for m in MUTATIONS:
            print(f"{m.ident:44s} {m.breaks}")
        return 0

    chosen = [m for m in MUTATIONS if args.only is None or m.ident == args.only]
    if not chosen:
        sys.exit(f"no mutation named {args.only}")

    needs_asan = any(m.sanitize for m in chosen)
    print(f"building baseline in {BUILD.name}" + (" and build-asan" if needs_asan else "") + " ...")
    r = configure(args.jobs, needs_asan)
    if r.returncode != 0:
        print(r.stdout[-4000:], r.stderr[-4000:])
        sys.exit("baseline build failed")

    survived, killed, broken = [], [], []

    for m in chosen:
        target = REPO / m.path
        original = target.read_text()

        # An anchor that no longer matches means the registry has drifted
        # from the code. That is a hard failure, not a skip: a mutation
        # that silently stops applying is exactly the kind of quiet
        # nothing this whole script exists to prevent.
        occurrences = original.count(m.find)
        if occurrences == 0:
            print(f"  BROKEN   {m.ident}: anchor not found in {m.path}")
            broken.append(m.ident)
            continue
        if occurrences > 1:
            # An ambiguous anchor can land on dead code or on a path the
            # test never reaches, and the result is indistinguishable
            # from a test that would have caught it. This found real dead
            # code the first time it ran.
            print(f"  BROKEN   {m.ident}: anchor matches {occurrences} places in {m.path}, "
                  f"so the mutation could land anywhere")
            broken.append(m.ident)
            continue

        target.write_text(original.replace(m.find, m.replace, 1))
        try:
            b = build(args.jobs, m.sanitize)
            if b.returncode != 0:
                # A mutation that does not compile is useless as a test
                # of the tests. Treat it as a registry defect.
                print(f"  BROKEN   {m.ident}: mutant does not compile")
                broken.append(m.ident)
                continue

            killers = []
            for t in m.killers:
                rc = run_test(t, m.sanitize)
                if rc is None:
                    continue
                if rc != 0:
                    killers.append(t)
            if killers:
                print(f"  killed   {m.ident:42s} by {', '.join(killers)}")
                killed.append(m.ident)
            else:
                print(f"  SURVIVED {m.ident:42s} ({m.breaks})")
                survived.append(m.ident)
        finally:
            target.write_text(original)

    build(args.jobs, False)
    if needs_asan:
        build(args.jobs, True)

    print()
    print(f"killed {len(killed)}  survived {len(survived)}  broken {len(broken)}")
    if survived:
        print("\nSURVIVING MUTATIONS (an invariant nothing is guarding):")
        for s in survived:
            print(f"  {s}")
    if broken:
        print("\nBROKEN REGISTRY ENTRIES (anchor drifted or mutant will not build):")
        for b in broken:
            print(f"  {b}")
    return 1 if (survived or broken) else 0


if __name__ == "__main__":
    sys.exit(main())
