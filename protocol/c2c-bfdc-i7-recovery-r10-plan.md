# Punto product recovery, C2C plan revision 10

Task `c2c_bfdc`, iteration 7 remains active. Revision 10 changes only the
offline recovery contract, evaluator, tests and graph authorized by the C2C
revision-10 PLAN. Production, GTK/Chromium observers, host services,
installation, commits and release artifacts are unchanged. The accepted
`freeze_and_reframe` node and revision 5-9 evidence remain frozen.

## Correction-completion time origin

The undo and learning cases no longer use `Space.completed` as a proxy for
correction completion. Their temporal relation starts at a checkpoint-attached
confirmation of the visible automatic-correction transition. The existing
collector Ed25519 trust root attests the confirmation; no new signer, key,
service or product hook is introduced.

The signed payload binds task, iteration, plan, contract, run, environment,
artifact, provenance, case, attempt, checkpoint, observer identity and state
generation, trigger action and collector event, corrected-state digest, and
the earliest/latest collector-monotonic completion bounds. A missing, replayed,
foreign, malformed or incorrectly signed confirmation is invalid evidence.
The interval must remain between the signed trigger completion and the bound
checkpoint capture.

Confirmation IDs are unique across the complete primary-and-soak package.
An ID enters the package registry only after signature, binding, interval,
result digest and expected checkpoint state all validate. Invalid evidence,
including a correctly signed digest of a semantically wrong state, cannot
reserve an ID and poison a later valid confirmation.

For a maximum relation, all possible completion times must be within the bound.
For a minimum relation, all possible completion times must be beyond the bound.
An interval that straddles the threshold is inconclusive and yields
`INVALID_OBSERVATION`. The expiry case retains the requested whole-millisecond
2500/2501 ms test points; it does not claim nanosecond coverage for every value
strictly greater than 2500 ms.

Rapid-tail timing remains `Space.completed -> tail.started`; it is not coupled
to correction confirmation.

## Offline acceptance

The focused RED proves the old evaluator rejects a valid operation where
2600 ms elapsed after Space but only 2300 ms after signed correction completion.
The paired negative requires `correction + 2501 ms` to report
`temporal_constraint_exceeded`, preventing a false GREEN that merely accepts
the new schema and ignores its relation.

The full fixture retains 52 authorized cases, the unchanged repetition digest,
620 primary attempts and 200 soak operations per artifact, both baseline and
candidate identities, and `NOT_RUN_AUTHORIZATION` lifecycle results. It covers
2500/2501 boundaries, interval uncertainty, missing and forged confirmations,
full binding mutations, delayed correction, primary and soak enforcement, and
the previously accepted F1/F3/F4 regressions. Review-derived regressions also
prove package-wide replay rejection, invalid-record non-poisoning, and
fail-closed suppression of dependent product failures when the correction
anchor is missing, foreign, late or attached to the wrong action.

The final offline suite passes 89/89 tests. Independent code and architecture
review both pass the final source. This advances only the recovery-contract
node to C2C review; it does not advance either GUI probe.

This is evaluator-contract evidence only. It does not prove the internal
`undo_applied_at_` timestamp, physical-input behavior, GTK/Chromium producer
readiness, installation or product acceptance. Both GUI probes remain
`BLOCKED_ENVIRONMENT`; GNOME Shell, IBus and udevmon must not be restarted.
