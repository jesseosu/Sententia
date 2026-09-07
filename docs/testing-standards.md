# Testing Standards

Rules this project follows, each one written because it was broken first.
The failure that produced each rule is in `docs/failure-modes.md`.

The short version: **a green suite proves very little on its own.** It
proves the tests agree with the code, which is also true when both are
wrong, and true when a test cannot observe the thing it is named after.
Three mechanisms exist here to check the tests themselves.

## The three gates

| Gate | Question it answers | Run it |
|------|--------------------|--------|
| `make mutants` | Can these tests fail? | before merging a phase |
| `make flake` | Do they pass for reasons in the code? | before merging a phase |
| `make asan` | Is anything wrong that no assertion can see? | before merging a phase |

All three run in CI.

---

## 1. Every "this never happens" test needs a mutation

A test asserting that something bad does not occur is worthless until you
have watched it detect the bad thing occurring.

This project has had two tests that passed while being structurally
incapable of failing. One built a world where the bug it was named after
could not be expressed, then confirmed it did not occur. Its comment
confidently described a scenario the setup made impossible.

`scripts/mutation_check.py` breaks the code in 20 named ways and asserts
that a named test notices each one. A survivor means either the test is
vacuous or the invariant is unguarded.

```
$ make mutants
  killed   engine-time-priority          by test_invariants
  killed   election-majority-of-one      by test_election_sim, test_election
  ...
killed 20  survived 0  broken 0
```

The first run found **four unguarded invariants**, including Raft's
Figure 8 rule, which is the subtlest correctness property in the project
and which nothing tested. It also found dead code, because a mutation
landed on a function that was no longer called.

**Adding an invariant means adding a mutation.** If you cannot write one
that a test kills, you have not really tested the invariant.

### Mutations must be unambiguous

An anchor matching more than one place in a file is a hard failure, not a
warning. A mutation that could land anywhere can land on dead code or on
a branch the test never reaches, and the result is indistinguishable from
a test that would have caught it.

### Mutations must compile

A mutant that does not build is reported as a broken registry entry.
`-Werror` makes this easy to trip: a mutation that leaves a variable
unused fails the build rather than the test.

---

## 2. Assert summaries, never instants

The most repeated mistake here, four times across five phases: bounding a
test by something the environment controls.

- Asserting a short write happens on loopback. It does not, even at
  50,000 messages, because kernel buffers absorb it.
- Bounding a demo by a fixed cycle budget that expires under CPU load.
- Asserting a queue is saturated *right now*, when the send path drains
  before checking.
- Then replacing that with a different instantaneous read one line lower,
  which flaked at exactly the same rate.

The distinction that works:

| Safe to assert | Not safe to assert |
|----------------|--------------------|
| A peak accumulated across a loop | A depth sampled once |
| A count of events that occurred | A condition true at this instant |
| A total, a checksum, a bound | Anything the kernel or scheduler decides |
| "Eventually X, driven in a loop" | "X, right now" |

If the property genuinely is instantaneous, drive it in a loop until it
holds and bound the loop by **iterations, not wall-clock time**. A test
bounded by a duration fails on a loaded CI machine for reasons unrelated
to the code.

`make flake` runs the suite ten times and fails if any run differs. A
single green run cannot tell "correct" from "correct on this machine when
nothing else was happening".

---

## 3. Agreement is not correctness

Replication tests compared the leader against the follower and nothing
else. That checks agreement. A bug breaking both nodes identically passes
every such assertion.

The mutation harness proved it: skipping one command in five hundred on
the apply path left every convergence test green, because both nodes
skipped the same ones and agreed perfectly on a wrong book.

So convergence tests now compare against a **third party**: a
single-process engine applying the same commands with no replication
involved. The cluster must match that, not just itself.

More generally: when two things are checked against each other, ask what
happens if both are wrong the same way.

---

## 3b. Pin the values you cite as evidence

A checksum quoted in a status report is not a test. For five phases this
project ended every report with "the replay checksum is unchanged,
therefore nothing regressed", while that value was asserted nowhere and
compared by eye. The check that looked like it guarded it only compared a
run against itself.

If a number is worth citing as evidence, pin it as a golden value so a
change fails loudly and has to be an explicit decision.

And pin it against an input that can actually detect the change. Every
crossing in the original sample order file happened at the same price, so
it could not have noticed the execution-price rule changing at all.
A golden value over a trivial input is a golden value over nothing.

---

## 4. Ports are 0, budgets are iterations, seeds are fixed

- **Bind port 0** and let the kernel assign. Fixed ports collide between
  concurrent runs, and a test that fails when another test is running is
  a test that fails in CI.
- **Bound loops by iterations**, not by time. Demo processes exit when the
  work is genuinely finished, with a cycle budget only as a hang guard.
- **Seed all randomness explicitly.** The election simulator runs 115
  randomised fault schedules, and every one replays exactly from its
  seed. Chaos that cannot be reproduced is an anecdote.

---

## 5. "Done" in a cluster means the cluster is done

A demo failed one run in eight because the leader's completion check
looked only at its own applied index, and it exited while a follower was
still one commit-advertisement behind.

Any code asking "have we finished" in a distributed system has to decide
whose finish it means. The local answer is almost never the interesting
one.

---

## 6. Sanitizers, because assertions cannot see memory errors

Mutation testing found a bounds check whose removal changed no test
result: the read ran past the end of a buffer and every assertion still
passed, because the garbage happened to fail a checksum anyway.

No amount of assertion writing catches that. `make asan` builds with
ASan and UBSan and runs the in-process suite. That mutation is now killed
by the sanitizer build rather than by an assertion.

---

## 7. Verify the edit landed

Not strictly a testing rule, but the same failure shape and it happened
five times: a substitution-based edit whose anchor had drifted, reporting
success and changing nothing. Once it landed in the document whose job is
cataloguing that mistake.

An edit that cannot fail is an edit you cannot trust. Assert the anchor
exists, apply, then verify the result independently. "I changed it" and
"it changed" are different claims and only the second one matters.

---

## 8. Look at CI, not just the local suite

Every phase of this project was verified locally on Linux with GCC and
Clang, and reported green. CI also builds on macOS and Windows, and was
red from Phase 4 onward for five commits without anyone noticing.

Two real macOS-only bugs were sitting there: `fdatasync` does not exist
on that platform, and a read loop gave up at the first `WouldBlock`
because on Linux loopback the bytes are always already there.

A local run tells you the code works on your machine. That is worth
knowing and it is not the same claim. Check the runs before calling a
phase done.

---

## Running everything

```bash
make test      # the suite once
make flake     # the suite ten times, fail on any difference
make mutants   # break the code 20 ways, assert the tests notice
make asan      # build and test under ASan and UBSan
```

CI runs all four.
