# Product restoration

This document records the source restoration through 2.8.10. Source tests, package
validation, publication and machine installation are separate evidence gates;
runtime health alone is not proof of an editor correction.

## Sound routing and retained Chromium PRIMARY (2026-09-06, 2.8.10)

The sound menu incorrectly called the general settings route, and its old test
expected that wrong title. SettingsDialog now presents a typed Sound section
with the existing checkbox and shared save/error handling. General settings
remain unchanged; a visible modal retains its unsaved draft on re-entry.

Native Chromium exposed a gap hidden by GTK fixtures: after replacing Punto's
temporary selection, Chromium keeps PRIMARY while the DOM caret is collapsed.
The first correction passed and subsequent manual/automatic corrections were
rejected. WordEditor now retains the verified prepared selection only after a
dispatched edit. A later word edit can admit that exact receipt after checking
owner, text, timestamp, ownership generation, focus and X11 session. New user
selections and runtime resets invalidate permission. EventLoop preserves this
completed receipt only when replacing a pending manual request; it does not own
selection-admission policy. ClipboardManager remains the evidence provider.

| Scenario | Acceptance criterion and actual seam |
| --- | --- |
| S1 | Actual tray activation opens only Sound controls with its dedicated title; General still opens the full dialog |
| S2 | Sound save changes the sound preference through existing Config persistence; the full unrelated-settings oracle is unchanged |
| S3 | Cancel, unchanged acceptance and persistence failure preserve the appropriate file/UI state |
| S4 | Modal re-entry and reopen preserve draft/saved state according to the existing singleton contract |
| C1 | Two native Chromium Pause operations toggle the exact DOM text twice |
| C2 | Two consecutive automatic corrections update exact DOM text and leave the expected keyboard layout |
| C3 | A new real same-client, same-text selection is rejected without changing DOM selection/text or native key count |
| C4 | Actual IPC status reset invalidates a retained receipt and rejection leaves the DOM unchanged |

Independent BDD critic, separate BDD auditor, pre-RED SRP, actual RED critic and
final combined source/SRP reviews passed. The old code fails C1/C2 while C3/C4
already pass; the sound RED fails on the dialog title. No further refactor was
needed after GREEN. One test-only file was added and none removed. Runtime
changes add 44 net lines for the new behavior (+192/-148); test changes add
244 net lines. CMake/CI add 15 lines. No new production module or library is
needed. No parallel persistence, UI-state or selection-policy owner was added;
broker/proxy decisions are N/A for these changes.

| Focused gate | Result and evidence |
| --- | --- |
| Sound RED | `/tmp/punto-sound-route-red.log` |
| Sound Release / sanitized Debug | PASS, `/tmp/punto-sound-route-green.log`, `/tmp/punto-sound-route-debug.log` |
| Chromium RED on old driver | C1/C2 fail, C3/C4 pass, `/tmp/punto-browser-probe-PO9CKA/chromium-red-final.log` |
| Chromium Release / Debug | 4/4 each, `/tmp/punto-browser-probe-PO9CKA/chromium-green-release.log`, `/tmp/punto-retained-primary-chromium-debug.log` |
| GTK/VTE Release / Debug | 86/86 each, `/tmp/punto-retained-primary-gtk-release.log`, `/tmp/punto-retained-primary-gtk-debug.log` |

The browser tests use a fresh native Chrome/Chromium profile in a private
networkless Bubblewrap/Xvfb session. They exercise EventLoop and the real editor
through the existing XTest relay, not the host's kernel interception pipeline.
No host editor buffers, clipboard or browser profile are used. CI requires the
browser suite; an absent browser is a failure there, not a successful skip.

This fixes repeated corrections after Punto's own retained selection, not
arbitrary stale same-client PRIMARY originating in another field. The latter
still fails closed. Host device-specific logs show rejected edit dispatches;
the user's particular editor failure is not yet proven to have this cause.
STATS counters belong to each daemon, not an aggregate over all keyboards.
The existing full-snapshot settings save can still overwrite a concurrent
external configuration edit; this release does not change that contract.
Exact-commit CI, final artifact checks and installed read-back are recorded
separately in the release delivery report.

### Original key release checkpoint

After the IPC fix removed observer interference, a private 25 ms Space-release
delay still produced Dispatched with a permanently missing trailing space.
Four seconds of fresh DOM snapshots ruled out early observation. A separate
XQueryKeymap probe showed Space becoming UP before the relay delivered its
original release: the macro pressed an already-held key (ignored by XTEST),
then released it. Evidence: `/tmp/punto-relay-space-after-ipc-fix.log` and
`/tmp/punto-keymap-space-causality.log`.

WordEditor now checks actual server key state before its first editor action,
waiting through its existing input-buffer/cancellation callback and absolute
300 ms deadline. Captured focus, group, locks, pointer and session are checked
on every iteration. Timeout/context change rejects before preparation; no user
key release is synthesized. All word, terminal, selection and native-undo paths
cross this initial fence. There is no new wait after a verified PRIMARY snapshot.

K1 delays real Space release 25 ms and asserts exact corrected DOM/caret/dispatch.
K2 holds Space beyond the macro budget, verifies rejection while the server still
reports the key DOWN and source/clipboard are unchanged, then releases the actual
queued event and proves Pause recovery. K3/K4 change focus or disable while a
test-only real query receipt and server key state prove entry into the wait.
Existing C2 covers already-released consecutive automatic edits unchanged.
Faults belong to a per-instance private relay; only held-key negative cases
disable Space autorepeat on their private Xvfb and restore it during cleanup.

Actual old-runtime K1/K2 RED: `/tmp/punto-key-release-red-final.log`. Final focused
Release/Debug pass 5/5 in `/tmp/punto-key-release-release-green.log` and
`/tmp/punto-key-release-debug-green.log`. Independent BDD critic, separate auditor,
pre-RED SRP, actual RED critic and final source/SRP reviews passed. WordEditor
owns admission/deadline, existing XCB helpers own bounded transport, and fixture
adapters supply evidence only; persistence/UI/broker policy are unchanged/N/A.
No further refactor was needed. Five existing files add 160 lines, of which 13
are production runtime; no file or parallel state owner is added.

This addresses ordinary keys already visible as held at initial preflight.
Held modifiers can still be rejected earlier by idle_layout; this is not a new
modifier-release recovery feature. Arbitrary external keydown after the fence
remains outside this fix. Source gates, final package and CI are separate below.

### Read-only IPC and macro cancellation evidence

The tray polls STATS every two seconds. Both STATS and GET_STATUS are queued
for EventLoop, but wait_and_buffer previously cancelled a macro for any queued
command. A real Unix request admitted during a held clipboard initialization
reproduces this: both read commands reject at context_changed and leave the
source word unchanged. This is a separate source-proven product defect, not
an asserted explanation of the earlier tray-free hosted Chromium failure.

The IPC layer now shares one read-only verb classifier between diagnostic
admission and an owner-side pending-mutation query. The query holds the existing
consumer guard and scans one acquire-loaded published ring interval without
draining, changing counters or executing callbacks. Unknown verbs, missing slots
and consumer contention are conservative. EventLoop still owns cancellation;
both wait checks now cancel for pending mutations, not read requests. Stop,
input bounds, FIFO order and the existing macro deadline are unchanged.

R1 covers GET_STATUS/STATS preserving exact GTK text correction and real replies.
R2 covers SET_STATUS, RELOAD and denied SHUTDOWN retaining cancellation and no
editor mutation. R3 covers all verbs, unknown verbs, writes behind reads,
wraparound, capacity, FIFO/completions, close with pending writes and an
unpublished stalled producer. R4 measures full replies below three seconds.
Consumer contention is source-reviewed, not deterministically forced in tests.
Test-only synchronization acknowledges enqueue after publication and expires
after 150 ms; tests reject expiry, so socket-send timing cannot create a false
pass. No hook is compiled into production.

Actual old-runtime RED: `/tmp/punto-ipc-macro-red.log` (two read failures, three
control negatives passing). The missing-API compilation failure in
`/tmp/punto-readonly-ipc-api-red.log` is secondary evidence only. Final focused
Release/Debug pass 5/5 in `/tmp/punto-ipc-macro-release-green.log` and
`/tmp/punto-ipc-macro-debug-green.log`; IPC contracts pass in
`/tmp/punto-readonly-ipc-unit-release.log` and
`/tmp/punto-readonly-ipc-unit-debug.log`.

Independent BDD critic, separate auditor, pre-RED SRP, RED critic and final
source/SRP reviews passed. IPC owns classification and queue observation;
EventLoop owns macro policy, and test adapters supply evidence only. Persistence,
UI state and broker routing are unchanged/N/A. No further refactor was needed.
Seven existing implementation/test/build files add 195/remove 4 lines. Excluding
test-only hooks, production changes add 22/remove 3 lines (net 19), buying the
non-interference property. No file, queue implementation or parallel state owner
is added. Final source CI and package/install evidence remain separate gates.

### Browser lifecycle and rejection diagnostics evidence

Hosted CI then exposed a test cleanup defect: Chrome's parent could exit while
descendants still wrote its private profile. The fixture now owns a separate
process group, drains live members with bounded TERM/KILL, and deletes the
profile only after successful shutdown. Zombie-only groups cannot write and do
not block cleanup. A failed stop retains the profile. Existing correction
assertions and C2-first ordering remain unchanged.

Lifecycle scenarios cover an orphan writer ignoring TERM, survival of an
unrelated group, exited/failed startup, repeated cleanup, and profile preservation
on stop failure. The actual old cleanup fails the live-writer assertion in
`/tmp/punto-browser-cleanup-red.log`. Final focused tests pass 4/4 in
`/tmp/punto-browser-cleanup-final-green.log`; Release and Debug aggregates pass
8/8 in `/tmp/punto-browser-cleanup-release8.log` and
`/tmp/punto-browser-cleanup-debug8.log`. The fixture owns child/profile lifecycle;
private proc inspection and signaling are mechanical helpers. Production state,
persistence, UI and broker policy are N/A. One test file adds 145/removes 13 lines.

The remaining intermittent automatic rejection was opaque (`status=0`).
WordEditor now classifies rejected operations with static `rejection_stage`
labels; EventLoop only projects the label in its existing status line. Labels
identify a check group, not a proven individual predicate or user-editor cause.
No text, clipboard content or window identifiers are logged. Admission conditions,
execution ordering, deadlines and retry behavior are unchanged.

R1 maps existing real keymap/context rejection cases to exact stage assertions
and unchanged no-mutation checks. R2 checks successful recovery without a stage
field. R3 checks private sentinels against full captured stderr. Both old-driver
tests fail solely on missing stage metadata in `/tmp/punto-rejection-stage-red.log`;
both pass in `/tmp/punto-rejection-stage-green.log`. WordEditor owns diagnostic
classification, EventLoop transports it; other service slices are unchanged/N/A.
All stage values are literals. There is no duplicate policy or new abstraction.
This diagnostic capability adds 32/removes 2 runtime lines in three existing
files, plus 11 test lines; no production file is added or removed.

For both changes, independent BDD critic, separate BDD auditor, pre-RED SRP,
actual RED critic and final source/SRP reviews passed. No additional refactor
was needed. Broader final-source CI and package evidence remain separate gates.

### Initialization deadline checkpoint

The first hosted run failed the initial browser startup at the fixture's
15-second deadline; subsequent browser cases passed. The fixture now allows
45 seconds within the existing sandbox/CTest bounds. A real browser delayed
16 seconds fails the old readiness check and passes the new one; exited and
no-window children still fail and are reaped. Production deadlines are unchanged.

A subsequent hosted Release run rejected the first automatic edit before text
preparation; sanitized Debug passed. Local diagnostic trials did not reproduce
that exact rejection, so its cause is not asserted. Inspection did expose a
separate budget coupling: ClipboardManager initialization shared the 10 ms
per-request timeout across connection setup, atom requests, extension setup and
selection baselines. A controlled 20 ms initialization delay reproduced rejection
of a valid native-browser correction before any text change.

WordEditor now passes its existing absolute 300 ms macro deadline to
ClipboardManager::open(deadline). ClipboardManager remains the initialization
owner; default open() and ordinary request timeouts retain their previous budget.
The overload neither starts a fresh macro budget nor changes selection policy.
Already-open reuse preserves ownership, even if a new initialization deadline
has expired. Unopened expired calls fail before initialization.

| Scenario | Proof at the owning boundary |
| --- | --- |
| C5: valid cold initialization can exceed 10 ms | Native Chrome, one-shot 20 ms initialization delay, exact word/caret/dispatch/PRIMARY oracles |
| Caller budget, not request budget, bounds initialization | Public open(deadline), real private X11Session, 20 ms stopped-Xvfb atom and extension stages, 100 ms caller budget and 10 ms requests |
| Expired/short budget is not restarted | Expired call emits no initialization; stopped Xvfb with 5 ms remaining fails before a 100 ms rescue |
| Default open and request timeouts remain unchanged | Default 10 ms initialization fails on the 20 ms stall; an owned clipboard verification against stopped Xvfb still fails on the 10 ms request budget |
| Reuse is idempotent | Repeated open preserves the existing owned generation |

Independent BDD critic, separate BDD auditor, pre-RED SRP, actual RED critic and
final combined source/SRP reviews passed. The request-timeout test first owns
CLIPBOARD, because verify_ownership otherwise performs no server I/O. The browser
delay is only a scheduling probe; actual stopped-server checks prove bounded I/O.
RED: /tmp/punto-init-deadline-browser-red-final.log (delay reached, edit rejected),
plus /tmp/punto-init-deadline-api-red-build.log (new overload absent).
GREEN: /tmp/punto-init-deadline-browser-green.log and
/tmp/punto-init-deadline-clipboard-focused-green.log;
full clipboard contract: /tmp/punto-init-deadline-clipboard-green.log.
The API compilation failure alone is not behavioral proof.

Runtime grows by five net lines in three existing files. Test-only fault
injection uses the existing private marker and linker-wrap mechanism; it is not
compiled into the production daemon. No new production module, persistence
owner, fallback or retry was added. UI and broker/proxy decisions are unchanged
or N/A. No further refactor was needed. This runtime change invalidates the
earlier package checksum and requires fresh source, artifact and installed gates.

Final local source coverage: Release 25/25 (24 remaining CTest targets in
/tmp/punto-init-deadline-release-ctest.log plus the unchanged final clipboard
contract result above); Debug 26/26 (25 in
/tmp/punto-init-deadline-debug-ctest.log plus the separate GTK clipboard check in
/tmp/punto-init-deadline-debug-gtk-clipboard.log). Both builds include all 86
GTK/VTE cases and all five Chromium cases. The original automatic Chromium
scenario remains first, before the added initialization scenario, so the new
case does not prewarm that path on a fresh CI host. Package and exact-commit CI
are separate gates; these results do not certify the user's particular editor.

## Host-upgrade compatibility checkpoint (2026-09-05, 2.8.9)

The first candidate was installed for host validation but not published. That
check exposed an upgrade regression: a legacy root-owned 0644 exclusion file
with 248 printable ASCII tokens could not satisfy the new 0600, 128-entry,
alphabetic-only contract. The original file was backed up without changing its
bytes. No historical words were copied into tests or logs.

UndoDetector remains the only format, admission and persistence owner. It now
preserves legacy printable ASCII tokens and admits learned physical-key tokens
through the existing scancode map. Its bound is 1024 entries of 63 bytes, at
most 65536 bytes; the shared control-plane reader retains its 8192-byte default.
EventLoop uses the same physical-key mapping when looking up learned exclusions.
The installer only changes a trusted single-link root:root 0644 inode to 0600,
after checking its parent. It does not rewrite words or repair unsafe files.

| Scenario | Owner and proof |
| --- | --- |
| 248 legacy tokens survive read, learning and restart | UndoDetector synthetic full-set equality contract |
| 1024 maximum-size tokens fit; overflow and duplicates are bounded | UndoDetector capacity, file-size and small-read-limit contracts |
| Pending learning survives refresh and failed durability | Existing background-worker/concurrent-writer contracts at the new bound |
| Upgrade changes permissions without losing bytes or inode | Actual package lifecycle; links, FIFO and foreign owners remain unchanged |
| Wrong-layout punctuation learns after undo and restart | Real GTK `;tcn` correction, native key events, disk and editor-text oracles |
| Clean hosts run the same e2e without host configuration writes | Actual sandbox helper with absent and existing read-only host policy |

BDD critic, separate scenario auditor, pre-SRP and actual RED reviews passed.
The focused package lifecycle passed 87/87, and GTK learning passed both
alphabetic and punctuation cases. The sandbox contract passed both host layouts.
No new state machine, data-copy fallback, parser owner or broker was introduced.
The test gap was upgrade and clean-host coverage: the initial source suites used
fresh private files on a host where `/etc/punto` already existed.

RED evidence: `/tmp/punto-undo-legacy-red.log`,
`/tmp/punto-undo-duplicate-red.log`, `/tmp/punto-legacy-mode-red.log`,
`/tmp/punto-learning-punctuation-red.log`, `/tmp/punto-sandbox-missing-etc-red.log`.
Focused GREEN: `/tmp/punto-undo-legacy-final-green.log`,
`/tmp/punto-legacy-mode-green.log`, `/tmp/punto-learning-punctuation-green.log`,
`/tmp/punto-sandbox-cmake-green.log`. Two later fixture defects were reproduced:
interleaved diagnostic lines broke a readiness substring, and GTK's click
history turned a new drag into a double-click. Structured readiness retains a
wrong-layout negative control; the VTE fixture waits the actual GTK click
interval within its original deadline and asserts a single drag. Neither fix
changes production code, retries a correction or weakens PRIMARY/PTY checks.

Final source/SRP audits passed for all compatibility owners. Runtime C++ grows
by 6 net lines (+17/-11 in three files; EventLoop's guard is a net-zero change).
The installer adds 22 lines for the permission migration. There is one new
test-only sandbox contract and no new production file or service boundary.

| Final local gate | Result and evidence |
| --- | --- |
| Release | 24/24 covered: aggregate `/tmp/punto-compat-release-ctest.log`; final 86-case GTK/VTE rerun `/tmp/punto-compat-release-gtk-final.log`, PASS 55.54 s |
| Debug ASan/UBSan/LSan | 24/24 covered excluding GTK clipboard: aggregate `/tmp/punto-compat-debug-ctest.log`, with the sole failing fixture target superseded by `/tmp/punto-compat-debug-gtk-settled.log`, 86 cases PASS 58.85 s |
| Separate GTK clipboard | Existing unchanged-target ASan/UBSan proof `/tmp/punto-restored-debug-clipboard.log` reused; total Debug coverage 25/25 |
| CLI | 4483 checks reused for unchanged CLI/protocol source |
| clang-tidy and ShellCheck | Changed/dependent translation units PASS `/tmp/punto-compat-tidy.log`; unchanged translation units reuse the previous full check; all shell scripts PASS |
| Package migration | Actual postinst lifecycle 87/87 PASS `/tmp/punto-legacy-mode-green.log` |

Frozen EventLoop SHA256:
`d66c166699fc974c166e1398a3164eb1a18118b3c4d6b12651ed72caaf1205d3`.
Frozen UndoDetector SHA256:
`dcd8d32fe6b659ccead03b75e634c4d82edd4b2bafae7b04695750ac60f41b54`.
Final publication requires green CI for the exact release commit, including the
full packaging/reproducibility matrix, plus lifecycle checks on the final local
artifact. Release notes and the delivery report carry the published checksum
and host read-back; source health alone is not installation proof.

## Previous functional checkpoint (edbce08, first candidate 2.8.9)

The restored Release executor passes actual GTK selection layout/case/translit,
repeated word correction, immediate/native undo, CapsLock and CapsLock+NumLock,
and VTE canonical-PTY word and selection transformations. The frozen Release
passes 23/23 CTest targets; Debug passes 24/24 including the separately configured
GTK clipboard target. Both builds pass all 84 GTK/VTE scenarios. The earlier
GUI-classification assertion, stale undo learning and sequential PRIMARY races
are fixed. The gates below use 2.8.9 metadata and the restored sound UI/payload.

The pointer-state defect was a duplicate keyboard-group check: the executor's
own group switch changed QueryPointer's encoded group bits, aborting a replay
after its first character. Pointer checks now cover buttons/coordinates;
the existing keyboard observation remains authoritative for group and locks.
Cleanup pins the first observed suffix selection and will not collapse a
later same-client selection. Backspace preserves the corrected visible prefix;
repeated whitespace is tracked through the same delimiter path.

Undo learning now runs through one background storage owner. Tests cover cache
admission, the 128-word capacity, concurrent writers, and publication followed
by directory-fsync failure without replaying Add/Clear over a peer's changes.
The final focused contract passed in 5.24 seconds. Real GTK tests cover learned
exclusions after undo and restart, manual override, younger unfinished/mixed-layout
words, variable-length typo/undo, literal Tab, late clipboard receipt, foreign
copy recovery, bounded stdout failure, and incomplete input frames.

Sound UI RED failed on the disabled real menu, then GTK save/cancel/error/reopen
passed with private config and stubbed reload transport. SettingsDialog remains
the sole settings owner; duplicate tray sound state was removed. Package RED
proved the published artifact lacked both WAVs and a playback dependency. The
package builder now owns both assets for full and daemon-only flavors; extracted
artifact and missing/symlink input contracts guard that boundary.

Evidence: `/tmp/punto-restored-release-ctest.log`,
`/tmp/punto-classification-green.log`, `/tmp/punto-undo-final-green.log`,
`/tmp/punto-tray-sound-red.log`, `/tmp/punto-sound-package-red.log`,
`/tmp/punto-capability-cli-green.log` (4483 checks, zero failures).
Native BDD critic/auditor, focused pre/final SRP and actual RED reviews passed.
The external collaboration runtime was unavailable; no provider receipt is
claimed. Packaging and installation must use the frozen source and artifact
checksums rather than this earlier checkpoint's intermediate binaries.

Final audit found false learning after a rejected/cancelled reserved Ctrl+Z.
Three real GTK RED cases reproduced it. EventLoop now invalidates recent learning
at its existing cancellation/failure boundaries; successful undo still learns.
Ten focused GREEN cases cover the failures, positive learning and real VTE
selection. The VTE fixture uses mapped widget geometry and a real mouse drag,
not a timing-dependent double-click; exact PRIMARY and canonical PTY bytes
remain the acceptance oracles. No new state or API was added for this fix.

An aggregate mixed-history failure exposed a second lifecycle race: XTest
server acceptance preceded GTK releasing the selection prepared by the first
macro, so the next word hit the initial user-selection rejection. A real GTK
handler delayed by 80 ms reproduces this deterministically, without a runtime
test hook. WordEditor now waits read-only for its prepared owner to change,
within the existing 300 ms deadline and context cancellation checks. It does
not replay an action, collapse a newer selection, or redefine Dispatched as an
application acknowledgement. Twelve focused cases passed after the fix.
VTE gesture setup additionally waits for real ordered GDK press/motion/release
events, with the same overall deadline and guaranteed release in cleanup.

Evidence: `/tmp/punto-history-slow-gtk-red.log`,
`/tmp/punto-history-fence-green.log`, `/tmp/punto-vte-receipts-green.log`.

### Ownership and audit

| Responsibility | Authoritative owner | Verification |
| --- | --- | --- |
| Input order, history, undo admission, cancellation | EventLoop | GTK/VTE E2E, framing and stdout fault cases |
| X11 session/auth/context | X11Session | session and real XKB observation contracts |
| Bounded best-effort correction | WordEditor | actual editor text, focus, lock and stale-content cases |
| Clipboard payload, ownership and receipt | ClipboardManager | X11 contracts, delayed receipt and foreign-copy E2E |
| Learned exclusions and persistence transitions | UndoDetector | secure-file, capacity, peer-write and durability contracts |
| Settings UI and save errors | SettingsDialog | real GTK/save/cancel/reopen contracts |
| Runtime status and peer propagation | existing IPC/control plane | strict client contracts, CLI and daemon process tests |
| Sound playback | SoundManager | bounded asynchronous privilege-separated playback contract |
| Release files and dependencies | build-deb.sh | extracted full/daemon-only packages and lifecycle tests |

Tray and CLI project confirmed daemon state; they do not own correction or
learning policy. SettingsDialog does not introduce another YAML writer. Audit
evidence lives in the test suites and release logs, not in typed-text logs.
There is no broker/proxy or new service boundary in this desktop application.
Existing XCB/xkbcommon libraries own keyboard protocol and symbol handling.

Final native architecture audits passed after the focused RED/GREEN cycles.
The frozen runtime delta against 2.8.8 is 2 added files, none removed,
2460 added and 254 removed lines across cpp/include and cpp/src. This grows
the runtime to restore missing product capabilities and stronger failure
handling; it is not a code-reduction claim. The last undo failure fix adds
8 net runtime lines with no new state. The old KeyInjector is not linked into
production; WordEditor is the sole correction executor.

Known limits are explicit in README: ordinary X11 editor conventions, supported
US/RU groups, a 128-character word replay limit, bounded macro/clipboard I/O,
no application transaction acknowledgement, and terminal line-editor scope.
GTK/VTE proof must not be described as tests of every installed editor.

### Frozen source validation

| Gate | Result | Evidence | Invalidated by |
| --- | --- | --- | --- |
| Release CTest | 23/23 PASS, 92.67 s; 84 GTK/VTE scenarios | `/tmp/punto-final-release-ctest.log` | runtime, tests or build changes |
| Debug ASan/UBSan/LSan | 23/23 PASS, 103.90 s; 84 GTK/VTE scenarios | `/tmp/punto-final-debug-ctest.log` | runtime, tests or sanitizer environment changes |
| GTK clipboard sanitizer exception | PASS with ASan/UBSan; LSan disabled only for this target | `/tmp/punto-restored-debug-clipboard.log` | clipboard GTK target changes |
| CLI | 4483 checks PASS, reused for unchanged CLI source | `/tmp/punto-capability-cli-green.log` | CLI/protocol changes |
| clang-tidy CI profile | all 23 sources checked; final two changed sources PASS | `/tmp/punto-final-tidy-red.log`, `/tmp/punto-final-tidy-green.log` | source or compile-command changes |
| ShellCheck, diff whitespace | PASS | `/tmp/punto-final-shellcheck.log` | shell/source edits |
| Packaging contracts | 772 checks PASS, including reproducibility and both flavors | `/tmp/punto-final-packaging-contract.log` | packaging implementation or payload changes |

The tray's narrow external Fontconfig leak suppression retains a project-leak
negative control; no project-wide leak suppression is applied. Build logs contain
no compiler warnings. Production surface contracts verify the linked executor,
sound and learning owners and reject private fault-injection hooks.

Frozen runtime SHA256: EventLoop
`0e33039207c3b46d72fb497b2a8d854f2007b35b76ec03cd707c573d597308c0`,
WordEditor `181f83b19dc7281ae21b7fd0f36b677bdab5dd617d594def2d1fe82a3f037df2`,
UndoDetector `983a92b0421924e38b985b795c213ba772e4f28d671ff0b750da0310c1259c6c`.
These are source proofs, not published-asset or host-installation checksums.
Package tests build in disposable networkless namespaces and exercise fresh
installation, upgrade, failed setup, removal, purge and full-to-daemon transition.
The dependency-alternative delimiter defect was reproduced and corrected in the
metadata substitution owner before the successful packaging gate. No host package
installation or shared-service mutation is performed by those tests.

## Historical investigation below (superseded, not release requirements)

The checkpoints below retain the discovery trail. In particular, statements
requiring an editor plugin, disabling every mutation, excluding sound/learning
from production, or leaving the stale selection unresolved are obsolete. The
current contract is guarded best-effort X11 compatibility with 2.8.5, not a
universal editor transaction. See README for current behavior and limits.

## Recovery direction corrected after user feedback

The user confirmed that 2.8.5 worked in normal desktop use. Source comparison
identifies commit `794cacce87f9d089bb24f8d41186f83a5c7008fd` (2.8.6) as the
functional regression: it removed word/selection/undo dispatch and made
`SET_STATUS 1` return `Text mutation disabled`. Releases 2.8.7 and 2.8.8 retained
that disablement. Tests accepting swallowed hotkeys and unchanged text are
negative-path checks, not proof of a working replacement product.

The compatibility reference is its parent,
`3e957e11f3260f72bcf48553efa995887c7b4e14`, whose CMake and binary version are
2.8.5. There is no local `v2.8.5` tag; this source identity must not be confused
with a checksum-verified copy of the user's former installed binary. A detached
reference worktree at `/tmp/punto-285-reference-VhCMNu` builds successfully with
the current dependencies; its original CTest target passed (0.52 seconds).
This is build/unit evidence, not yet desktop E2E or deployment evidence.

Restore the existing user-visible modes against this reference, retaining
verified control-plane/authentication and failure-handling fixes. Classify
concrete reproducible defects separately from ordinary best-effort X11 limits.
Do not require universal editor-transaction guarantees or editor plugins as a
new prerequisite that silently replaces the requested Punto behavior. The
editor-adapter proposal later in this document is an optional architectural
alternative, not the approved recovery direction. The experimental WordEditor
selection hazard remains a real defect in that implementation; do not mistake
it for proof that all working 2.8.5 mechanisms must be disabled.

No blind downgrade, host installation, new release or restart has been done.
Current working-tree changes and unrelated user files are preserved.

Compatibility acceptance map from `3e957e1`:

| Input | Required observable result / baseline owner |
| --- | --- |
| Pause | Word layout inversion, trailing whitespace preserved; `action_invert_layout_word` |
| Ctrl+Pause | Word case inversion without layout change; `action_invert_case_word` |
| Shift+Pause | Selected-text layout inversion and layout toggle; `process_selection` |
| Alt+Pause | Selected-text case inversion; `process_selection` |
| LCtrl+LAlt+Pause | Selection transliteration, taking precedence over single modifiers |
| Space / Tab | Ordered automatic correction, including retained following text up to `max_rollback_words` |
| Ctrl+Z | Immediate Punto undo within 2500 ms and before another ordinary keypress; otherwise native undo passthrough |
| Terminal selection action | Insert transformed PRIMARY at the command-line cursor; do not rewrite scrollback |

The shared active baseline word/auto/undo helper is `replace_text_oneshot`.
GUI paste uses Shift+Insert; terminal paste uses Ctrl+Shift+V. Selection actions
use `process_selection`. Baseline `paste_text_oneshot` and
`action_auto_invert_word` have no callers and should not be revived as parallel
paths. Restoring source alone is insufficient: current production CMake excludes
KeyInjector/SoundManager/UndoDetector, and its surface contract enforces those
exclusions. Update linking and acceptance checks together with restored behavior.
The intended boundary is EventLoop mode/history/undo orchestration plus one
shared bounded replacement executor, retaining current control/auth safeguards.

Exact local validation entry points (from the repository root):

```sh
cmake --build cpp/build-release --target punto punto-event-loop-e2e-driver -j 4
python3 tests/test_production_surface_contract.py cpp/CMakeLists.txt cpp/build-release/punto
```

The CMake daemon target and build artifact are named `punto`; the package installs
that artifact as `/usr/bin/punto-daemon`. Use CTest's configured commands for the
remaining contracts rather than inferring arguments from installed filenames.

## Regression

v2.8.8 discards every analysis correction and consumes Pause without an action.
Runtime health and the previous no-mutation tests did not prove the product's
primary behavior. Both real-editor reproductions fail on its released driver:

- W1: `hELLo`, Ctrl+Pause must produce `HellO` in GTK Entry.
- W2: `ghbdtn`, Space must produce `привет ` and leave the Russian group active.

Commands: run `tests/test_event_loop_gtk_e2e.py` with the built EventLoop driver
and `PUNTO_EVENT_LOOP_E2E_TEST` set to
`test_word_case_hotkey_changes_real_editor_text` or
`test_auto_layout_changes_real_editor_text`. Both failed at the correction
assertion after successfully typing the original word on 2026-09-05. W2 also
observed `need_switch=1`, so the failure is after classification.

## First stage: bounded GUI word editing

The new executor validates the keyboard map, acquires the existing session and
macro leases, selects the expected suffix, and checks PRIMARY before replaying
the replacement through XTest. It never takes CLIPBOARD ownership. Dispatch is
not an application acknowledgement and does not create an undo record.

This experimental stage supports only the existing tested GUI classification,
single-line mapped text, and an initially unowned or foreign-owned PRIMARY.
It does not yet satisfy the full product contract.

| Scenario | Acceptance | Test seam |
| --- | --- | --- |
| W1 | Real case change; one action for repeats | GTK EventLoop E2E |
| W2 | Real automatic layout change; next typed key uses target group | GTK EventLoop E2E |
| W3 | Stale content, caret or focus does not delete document text | GTK EventLoop E2E |
| W4 | Unsupported map, initial selection, modifiers and transport errors reject | Editor contract / GTK E2E |
| W5 | CLIPBOARD ownership and all offered data remain unchanged | X11 selection contract / GTK E2E |
| W6 | Wait for modifier release; repeat/release-only frames do not trigger | GTK EventLoop E2E |
| W7 | New queued physical input cancels a pending correction | GTK EventLoop E2E |

BDD critic, separate BDD audit and pre-RED SRP audit: PASS for this bounded
experimental stage. Independent RED review: PASS for W1; W2 subsequently failed
at its expected correction assertion. These are design/test reviews, not a
product-readiness approval. The agent-collab runtime was unavailable; no external
provider review receipt is claimed.

Ownership: EventLoop owns request admission, freshness and pending transitions;
WordEditor owns desktop preconditions and bounded transport; text_processor owns
pure transformations; Config and the existing control plane own settings.
ClipboardManager remains a mechanical selection reader, not a second mutation
orchestrator. No new persistence, UI state or broker policy is introduced in
this stage; diagnostics belong to EventLoop. The later checkpoint records the
completed bounded-stage SRP/regression gates; full product approval is pending.

## Required before product release

- Complete W1-W7, including actual keymap/CapsLock, failure after selection,
  focus changes, same-widget caret changes and second-keyboard input.
- All documented word and selection hotkeys, punctuation/whitespace, repeated
  corrections, immediate undo and normal application undo.
- Bind selection transformations to the focused editable widget; PRIMARY
  client identity alone is insufficient for two widgets in the same process.
- Prove terminal word behavior separately. A terminal selection may be
  scrollback and must never be treated as a replaceable command line.
- Restore truthful status/control/tray/sound behavior and capability reporting.
- The fresh-layout stage below replaces the three-second discovery snapshot for
  mutation candidates, and the NumLock stage supports neutral NumLock maps.
  CapsLock/keypad state is not yet correctly represented; layout changes within
  a word remain unproved. These are functional release blockers, not rare cases.
- Keep known limits explicit: X11 is not an editor transaction protocol;
  transport failures after dispatch must not trigger blind replay or rollback.
- Refresh dependencies, packaging, documentation and release gates; validate
  the exact release artifact, then publish and install it.

No old runtime downgrade, old clipboard-paste fallback or changes to user config
are part of this restoration. Existing unrelated `repo-c4.json` work is retained.

## Runtime control restoration

The draft now separates runtime automatic correction from the immutable file
settings. `GET_STATUS` and `STATS enabled` report the runtime automatic-correction
switch; `configured_enabled` reports the file setting. Manual word hotkeys remain
available while automatic correction is disabled. Changing status cancels pending
edits accepted before that command.

`STATS text_mutation=experimental_gui_word` describes this limited executor, not
universal desktop support. `word_dispatches` counts server-accepted sequences;
it does not prove application acknowledgement. The legacy `corrections` field
remains zero because acknowledged correction/undo records are not implemented.
`pending_words` reflects the currently queued candidate/request. Legacy macro
timing and tail metrics are not implemented in this draft; zero is not a measured
zero-cost operation. Telemetry no longer says `xkb_set=off` after actual changes.

The EventLoop commits config/status candidates only after publishing shared
state. The writer reports three distinct outcomes:

- `NotPublished`: preserve local and shared values; report an error.
- `PublishedNotDurable`: adopt the visible values locally, but report an error
  because directory durability was not confirmed.
- `Durable`: adopt the values and report success.

The first user configuration supplies defaults when no shared/IPC runtime intent
has been established. Subsequent X11 refreshes preserve that intent. Explicit
RELOAD applies file defaults unless a newer SET was accepted after reload
admission. Secondary reload and promotion preserve authoritative shared status;
a rejected config path cannot prevent a secondary from applying OFF.

Real-editor tests cover pending cancellation, disable/enable, manual correction
while disabled, delayed config completion, X11 refresh, failed publication,
post-rename directory-fsync failure, and recovery. Process contracts also cover
shared startup authority, generation exhaustion, readonly secondary IPC and
primary failover. Fault injection exists only in test targets.

Independent BDD, precise RED review and final bounded control-stage SRP review:
PASS. These reviews do not approve the unfinished product modes above.

## Validation ledger

| Gate | Inputs / artifact | Environment | Result / evidence | Validity |
| --- | --- | --- | --- | --- |
| W1/W2 RED | Released v2.8.8 EventLoop driver | Private bwrap + Xvfb + GTK3 | Both failed at text correction; original input confirmed | Historical regression proof |
| W1/W2 focused GREEN | Driver SHA256 `ad321a725ae21551c7f6b0b9e97be371875855bd7ab0b4c356ba9a6ccf0aff52` | Same private desktop | Case, auto and following Russian key passed | Invalidated by subsequent executor transport hardening |
| GTK suite | Same driver; test SHA256 `479d41013ad0907a7dba899342efbe9dddc3a94bc54917741ebe9e2c9b9e56ba` | CTest / private desktop | 23/24 passed; async stderr observation race in existing config-session case | Test synchronization fixed; rerun pending |

The configuration-session test retains its exact IPC state and path assertions,
but awaits delivery of the expected log line before reading the drain buffer.
Independent BDD critic/audit, SRP and RED review passed for that test-only change.

Additional fixture regressions found during the ordered suite:

- Replacing an XKB map does not reset its locked group. Each scenario now resets
  both, preventing an earlier successful correction from changing later input.
- An unread Xvfb stderr pipe filled with xkbcomp warnings and blocked the 28th map
  update (isolated reproduction without Punto or GTK, 65,219 pending bytes).
  Diagnostics now use an anonymous temporary file with bounded startup reads;
  the repeated-map regression completes 40 updates and verifies editor input.

Source gate snapshot before the fixture-only stderr correction: Release daemon
SHA256 `12098770cbfd975b64ed3ea5aa02b6c0b81bc9b2d5d10f664383e351e6a0da4e`,
driver SHA256 `25ff1b5323ea1b0faf38958dd822bcc8666cb02a244971068f6ba6be007267c2`.
22/23 CTest targets passed, including all 16 process cases; GTK passed 35/36 and
stopped in test-server map setup before exercising the last product scenario.
The corrected full GTK gate passed all 37 cases in both Release and Debug with
ASan/UBSan and LeakSanitizer enabled for the daemon. Final test script SHA256:
`d4441e5025914030b3d4f0d8e9008e2b1e8d6be8b4dabd3d68fbdbdeb6440db9`.
The combined-correction test now waits for config completion, not merely request
admission, before typing. Its original premature wait failed under sanitizer
timing; the corrected focused test and both complete GTK suites passed.

The separate GTK clipboard contract passes ASan/UBSan using the existing CI
exception `detect_leaks=0`: GTK/fontconfig process-global allocations fail a
raw all-target leak run. This exception does not apply to the daemon or other
contracts. The final 22-target Debug run passed with the CI options
`detect_leaks=1:halt_on_error=1` and UBSan `halt_on_error=1`; the separate GTK
clipboard ASan/UBSan target also passed. All 23 Release targets have passing
evidence on the frozen runtime: 22 from the aggregate and the corrected complete
GTK rerun. The later edits changed only the GTK fixture and documentation.

Runtime surface versus the original checkout: 2 files added, 0 removed, 4
modified; +691/-54 lines. Runtime code increased to restore a product capability
and enforce publication outcomes; this is not a code-reduction refactor. The
legacy clipboard-paste executor was not reintroduced into production.

The installed package was checked again: `punto-switcher 2.8.8`, daemon SHA256
`dca352e277441bd47df9d7ced97a64cf9becf97047aaceb773ba65731a1fec10`, runtime
`text_mutation=disabled enabled=0 configured_enabled=1`. No release, installation,
service restart, credential change or desktop text mutation was performed by
this restoration stage. All synthetic input stayed inside private Xvfb.

## NumLock restoration, 2026-09-05

WordEditor now accepts NumLock only when its real modifier row contains no other
meaning and the XKB types of every used key are independent of those lock bits.
The check includes every declared group of source, replacement, Shift and Left
keys. Generated libxcb accessors own wire parsing; no custom XKB evaluator,
thread, cache, dependency or public API was introduced. Unknown type indices or
unresolved virtual modifiers reject the plan before selection. Context checks
compare the exact initial locked mask throughout replay; the mask is not cleared.

| Scenario | Observable contract / test |
| --- | --- |
| N1 | Physical NumLock in Mod2; `ghbdtn ` becomes `привет `; next word is Russian; locks and CLIPBOARD unchanged |
| N2 | NumLock remapped to Mod3; the same correction and exact lock preservation |
| N3 | NumLock plus CapsLock or mixed Alt/NumLock row: unchanged text/CLIPBOARD, no Left event or selection |
| N4 | Only the replacement-side Russian key uses KEYPAD type: XkbLookupKeySym proves its NumLock-dependent case; reject before selection; ordinary-map retry corrects the text |
| N5 | New GetMap query fails in the test driver: marker consumed, no selection/mutation; retry corrects the text and retains NumLock |

All scenarios use the private bwrap/Xvfb GTK EventLoop test seam. Test setup
resets all locked modifiers and XKB options. N2 uses xmodmap to clear both rows:
an XKB `modifier_map None` declaration left the old NumLock action active and
was discarded as an invalid fixture. The physical NumLock key and exact server
mask assertions verify the final fixture before typing.

Independent BDD critic, separate BDD auditor, pre-RED SRP, RED critic and final
source/SRP reviews passed. N1 and N2 failed on the prior binary with unchanged
`ghbdtn ` and dispatch status 0, then passed after implementation. N3/N4 retain
existing fail-closed behavior; their positive recovery prevents an unrelated
permanent rejection from passing. N5's invalid-cookie injection proves query
failure/recovery, not a stalled X server or a new hard wall-clock guarantee.

Responsibility map: WordEditor owns lock admission and replay context; its
KeyboardPlan/type check mechanically proves the map precondition. EventLoop
retains request admission and dispatch diagnostics. Persistence, UI state and
broker decisions are N/A for this stage. Tests own fixture maps and state only;
the adapter/decision-duplication review found no second mutation authority.
Refactor: reuse existing bounded reply and generated map accessors; no further
safe simplification identified. Runtime delta: word_editor.cpp +136/-0 lines,
no runtime files added/removed, stronger lock correctness rather than code reduction.

Final post-refactor GTK gate: 43/43 Release (25.719 s), 43/43 Debug (26.948 s)
with ASan/UBSan and LeakSanitizer enabled. Includes the prior directory-fsync
fault tests after reuse of their private marker helper by N5. Frozen inputs:

```
d6adbcc36dc569a5ea0440ee82d831bd8af3e5a1951fcf1eb0f7df29ae4728d6  word_editor.cpp
dd048c1ed14840700a78853a17d31d1b082b3baefcfd6a78b3be98daa058b489  punto_event_loop_e2e_driver.cpp
94e83d7f4aed6a2a4c94223ebff77ccd0417409b3c6cf26ef474496d83620c30  test_event_loop_gtk_e2e.py
6a1b2a8157c31a909209cb80c68d093e62b357e52211b06d3f5b87f735afc986  Release punto
c14717772abb107f077f667107cfcadfc61199d54035b38e333b8e9a07440812  Release GTK driver
3539fb7d6a3ba61af8edd0bd16ae63b281b045587bf63fc820bb669aaa87c74d  Debug punto
199c6307e95dd6548ad226cb8ba71c8f7ffff3f4c207dee0d804c909cd5676ef  Debug GTK driver
```

This gate does not cover arbitrary XKB actions, live map changes during replay,
CapsLock, keypad input, widget-level provenance or the unfinished product modes.
The XKB type-neutrality rule follows the documented
[XKB modifier/type semantics](https://xkeyboard-config.pages.freedesktop.org/website/doc/enhancing/).

## Next input-correctness stage

A candidate-specific asynchronous keyboard observation now shares the existing
X11 background lane. It reads a bounded GetState reply from a committed session
snapshot without NSS/discovery or a held write lease during I/O. Typed consumers
cannot consume each other's jobs. Stable session authority generation is separate
from background-job generation. No public STATS field was added for test timing.

EventLoop captures raw words and admits one analysis task after a fresh response;
only the matching fresh response permits a mutation candidate. Superseded or
failed observations retain diagnostic-only analysis using the captured config.
WordEditor compares source group and session generation before selecting text.
Layout-only discovery updates no longer reload configuration.

L1/L2 RED confirmed `руддщ` remained unchanged after an immediate external RU
switch; manual mode also left a selection. Both focused tests now pass actual
`hello` replacement and subsequent English input within one second. L3 RED
confirmed an unnecessary config generation increment (2 versus 1); the corrected
layout-only refresh retains config generation/load count and runtime OFF.
L4 (group changed after observation) and L5 (new input during delayed observation)
pass without stale mutation. L6 checks stalled transport and recovery. Expanded
SET/RELOAD tests exposed a diagnostic result being submitted immediately before
its analysis epoch was discarded. Epoch cancellation now precedes diagnostic
admission. RELOAD cuts the epoch once at admission; completion clears mutation
eligibility without discarding the diagnostic work accepted during loading.
Both control cases retain exactly one analyzed result and reject the old edit.
RELOAD recovery uses a new physical word because successful configuration
commit deliberately clears the previous input history. Its oracle requires
`config_result=ok` and an advanced config generation; SET preserves that
generation. Independent source/SRP and final test-critic reviews passed.

The driver-only observation seam binds its marker to the exact GetState
connection/sequence and clears tracking on disconnect. Delayed reply delivery is
separate from the real socket-handshake blackhole test. Recovery uses a hard link
to the private Xvfb socket inode, retaining the production rejection of symlink
endpoints. These tests do not access the user's desktop.

Session discovery is not input acknowledgement. A candidate-specific snapshot
must be fenced against newer physical input, and one end-of-word snapshot cannot
reconstruct a layout/lock change within the word. Preserve mismatch rejection;
do not compensate with blind selection retries or a longer fixed sleep.

## CLI and tray integration, 2026-09-05

The old CLI rejected the new daemon's STATS schema and could roll back a fresh
backend start. It now strictly accepts the old 30-token disabled schema and the
31-token experimental GUI-word schema, including `word_dispatches`. Unknown
capabilities, malformed/reordered/extra fields and invalid numeric values still
fail validation. Legacy disabled state cannot report enabled automation.

The tray reads a typed capability/status snapshot in its existing single-flight
worker. Its toggle controls only automatic correction; the menu, settings and
About text identify the limited experimental mode. Sound remains unavailable.
Commands require the exact expected acknowledgement, then STATS supplies the
displayed state even after a failed command. A failed readback is unknown, never
optimistically enabled. Programmatic checkbox updates do not send commands.

BDD/owner map: daemon owns runtime state and persistence; IpcClient owns protocol
decoding, CLI owns service lifecycle, TrayApp owns menu projection and the
single-flight request lifecycle. Views do not decide backend capability.
SettingsDialog keeps file-config ownership separate from the runtime toggle.
There is no new broker/persistence authority. Independent critic, separate BDD
audit, pre/final SRP and actual RED reviews passed. RED: 151 CLI checks exposed
18 failures, including rollback of a valid start; GTK menu and About tests
reproduced false disabled labels. Final Release CLI: 4483/4483; IPC/GTK contracts:
2/2. Shell syntax, ShellCheck and whitespace checks passed. Runtime/test scope:
9 files changed, 0 added/removed, +484/-35 lines; capability restoration, not a
code-reduction refactor.

Debug IPC passed with LeakSanitizer enabled. The real-widget tray test initially
reported 62,251 bytes / 2,327 allocations. Releasing the current-thread Pango font
map after widget teardown reduced this to the external `FcInit` cache (320 bytes
in 3 allocations). Only the tray-app target suppresses that function; other
allocations remain checked. A durable CTest negative control uses the same binary
and options and requires detection of an intentional 73-byte project allocation,
its named stack frame and the exact leak summary. Configured Debug IPC, GTK and
negative-control gates passed 3/3 with `detect_leaks=1`. Independent fixture
BDD/SRP/oracle review passed; `pc_a84bd16ecea1` is resolved. The 320-byte cache was
excluded, not freed; no blanket leak exemption was added.

Live check during this stage still reports the installed v2.8.8 capability
`text_mutation=disabled enabled=0 configured_enabled=1`, with three daemon peers.
Nothing from the working tree has been installed or published.

## Integrated checkpoint, 2026-09-05

Frozen fresh-layout runtime and final GTK script:

| Artifact | SHA256 |
| --- | --- |
| Release daemon | `30b694c915775158f85290e12840e9870c7b407e4f4957418b9943a7e98fa809` |
| Release EventLoop driver | `9bfffcb0c145aa228b88b563c4b6e041ca1b192b52e261ffbe92bbe69153c58e` |
| Debug daemon | `09147a59de95f8dadd5982c2ea85ecd627a1022897881be4d9b618e38e617830` |
| Debug EventLoop driver | `5bb77dbb371b3934e6bcd146c1fe0879f8477eb950916892ca7f0a58020f6786` |
| GTK script | `a9c1920d69e33c3b5ff46d9b64c31422e8e6ee8778146a0d98c2425d5cfdb8a4` |
| EventLoop source | `96b07393c25f1df1d669989c7a8da280afe082cfd897cedb87917900d7f04f91` |

Release: 22 non-EventLoop CTest targets passed in the aggregate; the final
EventLoop suite passed all 50 cases in 29.20 seconds. Debug: the other 22
non-clipboard-GTK targets passed in the aggregate, then the final 50-case
EventLoop suite passed in 30.66 seconds with ASan/UBSan/LSan enabled. The separate
clipboard-GTK target passed in 0.28 seconds with the previously documented
external-GTK leak exception. These are combined exact-input results, not a claim
that the initial aggregates had no failures.

The Debug aggregate exposed an initialization race in the RU diagnostic test:
X11 readiness did not imply dictionary/config readiness, and the initial config
commit could reset a partially typed word. The test now waits for committed
configuration, a ready analysis worker and the RU diagnostic before typing.
The unchanged Debug binary passed the focused test in 0.584 seconds and the
complete suite above. No product timing budget or downstream oracle was relaxed.
Independent BDD critic, separate audit, pre/final SRP and RED review passed;
the test owns initialization preconditions only. No additional runtime refactor
was warranted.

The fresh-layout stage changed six existing runtime files (+213 net lines) and
one existing X11 contract (+81 lines); it added no runtime file or dependency.
This restores a capability and strengthens stale-request rejection, rather than
claiming a code-reduction refactor. The overall restoration includes the earlier
new WordEditor implementation and remains incomplete.
The independent integrated architecture/SRP audit passed this checkpoint and
explicitly rejected deployment approval; it confirmed all six recorded hashes
and the distinction between bounded-stage correctness and product readiness.

The nested Xvfb assertion was independently reproduced without Punto: Xvfb
21.1.12 aborts on the final XKB-client disconnect with XFIXES disabled. The
fixture now checks normal SIGTERM shutdown and explicitly terminates only that
variant with SIGKILL while its retained connection is alive. A pre-signal
child-alive check and exact exit-status check reject prior crashes. Two negative
controls cover earlier normal/signal death and idempotent reaping; destructor
failures reach the test result. Actual XFIXES absence is asserted, and existing
functional checks and xkbcomp warnings remain visible. This accommodates the
external bug; it does not fix Xvfb. Independent RED/SRP/oracle review passed.
Release passed in 3.01 seconds and Debug ASan/UBSan/LSan in 3.15 seconds without
assertion/abort. `pc_08b2e765eecc` is resolved. This fixture-only stage is +86/-8
lines; final file SHA256:
`73064ba7ff35ef5de4cfa390823d74db83790f22bef8e1e912919be9ef7631b0`.

### Deployment blocker

The stale-content GTK case still demonstrates a selected suffix remaining after
rejection. Subsequent user typing can replace that selection. Window/client
identity is also insufficient to distinguish two editable widgets in one
process. This is a release blocker (`pc_5276b28674a3`), not an accepted production
limitation. Do not install the experimental executor merely because its tested
successful corrections and process-health checks pass.

A proposed next boundary is a cooperating editor-side adapter that checks the
document revision, widget/caret generation and expected range, then changes the
text in the editor's UI thread without preliminary selection. Generic X11
selection ownership is not that contract. A local GTK prototype alone would not
prove integration with the user's actual editors; supported target applications
and their plugin/API integration must be explicit before deployment.

The host command-path check found VS Code, Cursor, Firefox, Google Chrome,
GNOME Text Editor and GNOME Terminal, but no `gedit` command. The GTK test's
`gedit` WM_CLASS is a controlled fixture, not proof that these installed
applications support the experimental executor. Choosing and integrating a real
editor target is a deployment decision; do not broaden the X11 allowlist from
this fixture evidence alone.

The latest host readback remains installed `punto-switcher 2.8.8`,
`text_mutation=disabled enabled=0`, three daemon peers. No new package, release,
service restart or host desktop input was performed at this checkpoint.
