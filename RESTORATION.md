# Product restoration

This document records the source restoration through the 2.8.14 candidate.
Source tests, package validation, publication and machine installation are
separate evidence gates; runtime health alone is not proof of an editor
correction.

## Publication integration, 2026-10-19

The default `master` includes the immutable v2.8.14 source, followed only by
completed documentation and test corrections. No runtime, package,
tag or uploaded asset is rebuilt or replaced by this integration.

The later tag-push run39153888171 differs from the successful branch run below:
Release49/50, sanitizer/package/tidy PASS. The failed Chromium test required an
old-child rejection and an untouched PRIMARY fault marker after external mouse
selection. The trace instead records origin_clear, PRIMARY timeout and safe
retained_selection_unreadable refusal. Typing-context invalidation is legitimate;
the exact differing pointer/button/focus component was not logged.

The test now uses physical Right before selection to clear old provenance,
requires the injected unreadable refusal and consumed marker, and keeps exact
DOM/clipboard/dispatch and released-key assertions. The neighboring stale-child
test remains. Deterministic precondition RED fails the old stage oracle; focused
Release2/2 and sanitizer2/2 validate the corrected test and its neighbor. The
failed tag result remains historical evidence, not a green rerun or waived gate.
Master CI is a separate integration gate; no production guard/deadline changed.

The first master run39203613330 passes CTest50/50, sanitizer50/50 plus clipboard1/1,
package and tidy, but CLI5315 records four assertions from one timeout: the
valid daemon_peers=0 case received ERROR timeout and its fixture saw no request.
The suite had forced50ms around a traced Bash/netcat process chain, unlike the
product default1000ms. Ordinary grammar/lifecycle cases now use that product
budget; the real50ms stalled-peer and25ms IPC checks remain explicit. A100ms
delayed valid peer is a causal regression: it fails the old50ms setup, then
returns one byte-exact request and response with the corrected setup. No retry,
production timeout change or successful-rerun claim erases the failed master run.

Run39206179947 exposes a separate Chromium retry-budget fixture assumption in
sanitizers: forced layout refusal returned PreparedNotReplayed/Never with its
range still selected, not the test's assumed Rejected/AfterFreshInput. That is
the conservative contract when cleanup cannot be proved; the exact failing
cleanup substep was not logged. The retry-accounting test now uses the existing
key-release protocol fault before selection preparation and requires the exact
safe rejection twice. Attempt limits, fresh-input gating and no-later-replay
checks remain; clipboard/layout/modifier assertions were added. Independent
post-layout retry and expired-cleanup tests remain unchanged. No runtime cleanup
deadline was increased and an unproved cleanup was not made retryable.

The same run's CLI5326 has one failure: stalled restart takes1460ms against a
1200ms instrumented wall allowance; five50ms IPC calls and all substantive state,
output and ownership assertions pass. Readiness policy uses a virtual40ms clock,
but process tracing and five IPC attempts consume real time. Stalled restart and
active-start now share the existing2200ms start/rollback execution allowance,
below the unchanged3s watchdog. Non-stalled wall limits remain unchanged. Every
stalled readiness case additionally requires the exact configured virtual
deadline, alongside five calls and their exact50ms timeouts. This distinguishes
policy proof from instrumented execution time; it changes no product deadline.

## Completed delivery, 2026-10-19 local time

[v2.8.14](https://github.com/antonshalin76/punto/releases/tag/v2.8.14) is published
by antonshalin76/id50358174. Tag source1817e90ce7da22462fe891805e25df6c7efd7e23;
native pre-push review01M582HV1DJFW48S65B525GT32 has no findings,124/124 paths.
Exact-head GitHub39152138346 passes all4 jobs: Release50/50, sanitizer50/50 plus
clipboard1/1, CLI5315/5315, package965/965, maintainer20/20, recovery431/431,
clang-tidy31TU and ShellCheck. No CTest skipped/unrun outcomes.

The corrected DEB7636aed0c57bd1e927dee9173790e527c0b2e147281ce2ecf7166917db4e875f
is installed and accepted; the earlier250718dd below is historical, not current.
Its build source is76362ee. Subsequent CI/test changes include14 C++ E2E-only
observation lines: current production rebuild plus package stripping is byte-for-
byte identical to the accepted daemon2aa8941f. Raw source identity is not claimed.
All4 published assets were downloaded, compared and checked against SHA256SUMS;
tag, author/uploaders, server digests and latest-release identity were read back.
Public BUILD_PROVENANCE records the source/build/CI/host distinction.

Read-only installed status is ready/enabled, backend/helper/tray active, queues
empty, live ELF correct. GNOME/IBus identities unchanged; rescue timers absent,
root-only backend-reinstall backup retained. Physical human-device validation and
a new long-idle soak were not performed. Unsupported private clipboard formats
and malformed Chrome non-BMP accessibility ranges still fail closed, as documented.
The checkbox plan is complete. Local delivery evidence resides under
`.cache/punto-product-20261017.WqYT4f/release-20261018/`.

## Historical implementation checkpoint, 2026-10-18

The active checkbox plan is
[product-delivery-plan-20261017.md](protocol/product-delivery-plan-20261017.md).
The2.8.14 release source is based on8f8e24a and assembled in an isolated release
worktree; existing dirty development changes are preserved. The Git tag and
release provenance identify published source and assets. Work continues without C2C.
Later user instructions authorized installation and manual relogin. At22:48
local time, exact DEB250718dd is installed and activated in X11 session11;
new bridge/helper readiness succeeds, and udevmon/backend/tray are running.
Owned GTK/Chrome critical checks pass: both manual directions, selection,
next glyph, empty recovery, auto Space, clipboard and released modifiers.
Current Chromium private clipboard targets cause documented safe Tab refusal;
no positive host Tab pass is claimed. Positive portable Tab coverage is reused
only from the qualified private suite. Events entered an existing installed
daemon input pipe, not physical human keys. Live Sound menu opens sound-only.
Independent payload/topology/data postflight passes; all4 config/learning files
retain bytes+uid/gid/mode/mtime. The r2 accepted marker is exact and its timer
is disabled. Backups remain private. No forced session/GNOME/IBus/GDM restart.
Host acceptance evidence: `.cache/punto-product-20261017.WqYT4f/deploy-20261018-r2/host-acceptance.md`.
Release preparation independently verifies the explicit PAT principal as
antonshalin76/id50358174. It is scoped to release commands; global credentials
are not switched. Source commit, pre-push review, hosted CI and published asset
readback remain separate from the installed host evidence above.

The later pre-push review exposed a real stale-PRIMARY defect: Shift+Pause
without a visible selection could append the previous word. This was reproduced
on the installed250718dd package; the earlier finite host pass did not cover
that sequence. Final source requires a nonempty live range matching PRIMARY and
revalidates it before mutation. All selection operations share this owner;
safe refusal retains ordinary-word recovery. Accessibility proof bounds match
the existing4096-byte selection domain instead of imposing a new256-character
limit. Current Chrome155 can return a collapsed AT-SPI range for a visible
non-BMP emoji selection; this inconsistent provider result is refused, not
converted using stale clipboard authority. BMP-unmapped paste and malformed
provider refusal/recovery have separate behavioral coverage.

Corrected-source checks, final package, backend/helper reinstall and publication
are recorded as successor evidence under the existing task. The backend-only
reinstall preserves a fresh quiescent four-file snapshot and exact prior package;
it does not reuse r2 markers or restart GNOME/IBus. This paragraph does not turn
pending checks into acceptance or change the historical250718dd results above.

### Earlier preparation checkpoint (superseded host state)

The exact installed2.8.13 package has been repacked and its three executables
compared byte-for-byte. A separate private archive preserves host configuration,
learning/reset data, permissions and service/extension files. An isolated
old-install -> candidate-upgrade -> exact-old-rollback rehearsal passes13/13,
including unchanged configuration/learning and removal of the candidate helper.
The archive is preparatory: live writers require refreshing and rechecking it
after quiescence in a future authorized cutover window. Host data stays private.

Earlier read-only host inspection found installed2.8.13, three per-device daemons,
an absent session-helper binary/unit and a loaded compatibility-v1 GNOME
extension without the required InputSource1 API. A source-CLI start correctly
refuses with `ERROR bridge-unavailable` without changing the daemon, GNOME
or IBus process identities. No package installation or session restart occurred.

The current batch uses bounded CMake parallelism, explicit OpenSSL build
dependencies, a native nonmutating helper readiness probe, and CLI-owned helper
startup before backend mutation. Test-only corrections restore protected
maintainer fixtures and make the private PTY test independent of its caller cwd.
Exact systemd ActiveState replaces the unsafe rc3-to-inactive assumption;
transitions and failed services require a command and bounded settlement.
The helper unit now uses ProtectHome=read-only: paired readonly transient-unit
probes fail with the old true policy and pass with read-only. Runtime/state
directories stay writable, home config does not. This establishes the namespace
access correction, not actual helper/bridge activation in the current session.
The active plan retains fresh results and unresolved review findings; the
historical package/installed counts below must not be promoted to these new bytes.

## Historical autonomous checkpoint, 2026-10-05

The user switched this work to autonomous Codex execution without C2C.
Historical R52 records below remain evidence, not current execution authority.
The delivery spine and safety gates are in
[product-delivery-plan-20261017.md](protocol/product-delivery-plan-20261017.md).

### Practical delivery checkpoint, 2026-10-05 10:17 UTC

Work is now feature-sized implementation and practical acceptance, not new
diagnostic campaigns or per-helper planning cycles. The existing installed
collector passes a separate base20 and then a fresh base20+200-operation soak
on the unchanged final DEB `d95f8a88650d6dc409c735626485583b0adc9c25ba38962040c18ea172bbd378`.
The fresh soak lasts1802.791s: three active blocks68/68/64 and two300s idle
intervals. All220 attempts are unique PASS; cleanup errors are absent. All140
source pins match at completion. Later documentation updates are checkpoint
successors, not a claim that those historical document hashes still match.

The final postflight independently checks unchanged six runtime writers and
GNOME/IBus, matching live daemon/helper binaries, original config bytes/mode/
owner/mtime, enabled status, absent learning/reset files, settled work queues,
released keys and retired GTK/Chromium/clipboard-reader observers. All six
clipboard representations match before retiring the intentionally finite
external fixture publisher; no six-format guarantee after its retirement is
claimed. The native owned-browser-reopen case also passes1/1 with unchanged
daemon/helper. Current observer dry tests pass221/221.

Independent audit accepts finite installed behavior/stability using the prior
78 completed product cases plus fresh220, since package, runtime and relevant
dependencies match. The failed prior packets stay failed. The earlier
first-auto refusal remains causally unexplained; successful repetitions do not
prove it fixed or establish that all potential defects are absent.

Evidence: `.cache/punto-delivery-final-20261005.WvlORX/final-base20-current-utf8worker01/`
and `final-soak-current-utf8worker01/`; soak summary SHA256
`97d1fd86bedb60bb078841ebfea8997c664c15e9ac8a5ead00ee4144b4def369`.
The exact postflight successor is `postflight-authority-exact-raw.json`,
SHA256 `d30674291f55220ea39bcc5b77158ef553305da4ad6778f5a74c43f19f66abcc`.
The earlier supplemental receipt rounded uint64 epoch/ns values and is retained
but superseded. Receipt producers must transfer Python JSON as raw text and
verify exact integer readback; JavaScript-number or jq numeric reserialization
is not admissible for those identifiers. Primary collector records already use
Python-native JSON. No product operation was repeated for this receipt fix.
After acceptance, only the admitted fixture unit
`punto-final-package-clipboard3-wvlorx.service` was stopped: MainPID732006
became0/inactive and the PID retired. The owned UTF8 sentinel remained readable;
six-format fidelity after publisher retirement is not asserted. A subsequent
maintained-facts readback again confirms unchanged pipeline/helper/tray/core,
configuration hashes, learning absence and enabled status.
No host installation, session restart, commit or publication is claimed.
Host activation still needs the current layout bridge loaded safely; GitHub
CLI remains the wrong principal, not authorized `antonshalin76`.

### Historical installed-package checkpoint, 2026-10-05 09:01 UTC

The final2.8.14 package was built once from the dirty-inclusive source snapshot:
SHA256 `d95f8a88650d6dc409c735626485583b0adc9c25ba38962040c18ea172bbd378`.
Its fresh installation passes on the disposable guest, including six old
writer deaths, package/conffile absence, exact config preservation and verified
new pipeline/helper/tray. GNOME Shell and IBus are unchanged. Host installation
and release remain gated by installed product/update/rollback acceptance.

The subsequent exact2.8.5 baseline is independently running and admitted.
Its incorrect status decoder and kernel-thread executable scan are corrected
and reviewed. The archived editor comparison remains NOT_VERIFIED: after its
first Pause the strict clipboard TIMESTAMP conversion is unavailable. The
archive is not modified, and current-package clipboard checks are not relaxed.
The retained failed comparison is not a legacy input PASS.

The real candidate starts before the deliberate failure42; rollback then
restores exact archived package/files, all legacy writers, original enabled
status and unchanged core. Normal upgrade on the same final artifact exits0
and verifies fresh candidate pipeline/helper/tray. These prove lifecycle and
restoration, not legacy GTK16 or final-package78/soak. Evidence is under
`.cache/punto-delivery-final-20261005.WvlORX/`. The complete installed attempt
finishes78 integrated operations (20 base,34 features,24 lifecycle including8
terminal) and six soak operations. The seventh soak Pause is dispatched, but
its post-Pause snapshot fails with GIO cancellation inside the100ms desktop
observation owner. The exact cancelled D-Bus method was not retained; product
causality is unproven. The failed summary remains failed, not overall PASS.
Original enabled status is independently restored, and separate readback
confirms final installed payload/pipeline, config hashes and unchanged core.
Complete original-state closure and a fresh1800s/200-operation soak remain
required. No host package or session was changed.

### Prior source/product checks, 2026-10-05 07:30 UTC

The current candidate uses the existing admitted UTF8 clipboard transport for
counted terminal word/case/inverse edits. Receipt admission precedes deletion;
same-generation clipboard retirement is required before success under the
original macro deadline. The distinct real-layout VTE profile passes16/16 on
daemon3ff460ee, WordEditor1058d21b and GTK testsbc32d771. Terminal Insert
admission does not constrain GUI word/case/selection operations.

Actual GNOME Terminal rejected the earlier CtrlShiftV after a desktop hop,
despite embedded VTE passing. The native VTE CtrlShiftInsert CLIPBOARD binding
now produces the expected terminal text, caret and desktop layout. All six
original clipboard representations are independently observed. The installed
candidate now passes all8 real terminal word/case/undo/refusal/automatic cases
and10 learning/reset/persistence cases. Exact-payload restoration permits the
existing fixture to reassert ownership before intentional Punto lifecycle
transitions; it does not imply manager-less publisher persistence.

The first sound observation missed players forked by SoundManager's worker
thread because it searched only the leader TID's children. Reusing the existing
process snapshot's kernel PPID projection fixes this demonstrated evidence
gap without changing playback. A real thread/fork regression has causal
RED/GREEN; the subsequent installed16 run passes10 learning cases and all6
ON/OFF/ON audio cases. ON records one exact daemon-child player and one nonzero
PCM stream; OFF records neither. Original files, absence, metadata, enabled
status and unchanged core identities are restored. This is partial installed
evidence, not complete78 or final-package acceptance.
Historical344 evidence below does not qualify these changed C++ bytes.

Current source gates pass Release34/34, ASan/UBSan Debug35/35 and Debug GTK
clipboard1/1. Canonical352 passes352/352 across13 packets, zero errors/skips/
omissions/retries/waivers and all165 inputs matching
(run-20261005T071607Z-166945a2). Strict static2 combines with29 unchanged
qualified units: only those modules include the changed header; other source/
header hashes and database61e805 match. The final collector suite passes419/419.
The complete installed attempt passed72 text/terminal/learning cases but
failed on sound-dialog mapping; a later sound subset passed12 cases but
repeated full system scans exhausted the original Save acknowledgement budget.
Both failed runs and independent original-state restoration are retained.
Bounded mapping and existing PID/epoch-bound public stats now have causal
RED/GREEN and independent source review; no playback implementation changed.
The next integrated78 acceptance will exercise the single final package,
including its1800s/200-operation soak, rather than repeat raw overlay78 after
each checker-only correction. Scoped product evidence is not package acceptance.

### Current bounded-observation recovery boundary

The formatted publication owner926266c2 passed current source/private gates,
but its complete native run459c8072 ended342/344 PASS, two failures, zero
errors/skips. Both failures reached the original layout deadline inside a focus
reply instead of the ordinary Wait path. Deterministic genuine focus/pointer
reply controls fail both stage assertions on unchanged WordEditor4d76e3ae.

Classification-only sourcea95bd804 then passes both stage assertions, but the
pointer/Tab case exposes a later automatic recovery failure. Its typed timeout
revokes prepared lineage, unlike Wait expiry; actual same-owner PRIMARY NONE
therefore cannot authorize the next fresh word. This failed partial GREEN
23243089 remains retained, not qualified as success.

WordEditor79c768f8 aligns only healthy, retained, uncancelled expiry with Wait
retirement before revocation. Known focus movement, real drift, transport and
session failures still refuse; exact cleanup/range/identity and fresh physical
input remain recovery authority. Final causal GREEN2/2 passes all refusal and
recovery tails in5.775s (34fe63a4). Independent BDD/mapping/SRP, actual final
source and GREEN reviews PASS. Runtime delta+31/-9, no new module/framework.
The combined known-focus-drift/pointer-timeout branch is source-verified only.

Current Release34/34 and ASan/UBSan Debug35/35 pass82.37s/96.97s; unchanged
DebugGTK1/1 is reused on exact ELF. Strict static31 combines current WordEditor1,
unchanged current CM1 and29 unchanged qualified translation units, with exact
compile database and43 unchanged headers. Native344 on these final bytes
passes344/344, all13 packets exit0, no errors/skips/omissions/drift/OOM or
retries/waivers. Receipt824ae290 ends01:22:58UTC; independent complete-log,
165-input and64-ELF terminal audit PASS. Installed78 and package/host/release
acceptance remain pending.

### Current conditional-publication boundary

The subsequent installed78 attempt passed33 operations, then the first GTK
literalTab refused at clipboard_ownership with642ms remaining. Text stayed
ghbdtn; the literal delimiter was absent. All eight original process identities,
configuration/enrollment and enabled status were independently verified after
the failed run. No lifecycle isolation or package phase began.

Real-X controls reproduce two ordinary-reader publication refusals on e7592e83:
guarded publication with an existing owner, and timestamp preparation for absent
CLIPBOARD while PRIMARY is locally owned. Initially reviewed a13e3426 keeps one shared
64-event budget and original deadline, serves OLD-generation reads outside the
guard, then refences the original owner before its single publication write.
Three causal controls and the complete publication-guard selector pass;
independent source/SRP review e4a3cd78 finds no blocker. Runtime delta is
66 additions/37 removals, with no new runtime module. Witness, INCR and
post-submission uncertainty remain unchanged. Formatted final926266c2 has
69 additions/37 removals; full ordinary/static gates passed on those bytes.
The subsequent342/344 failure led to the WordEditor correction above; current
native acceptance is receipt824ae290, not the preceding publication-owner run.

The corrected collector distinguishes a deadline expired before request dispatch
from an admitted pipe-write timeout. Complete affected private controls390/390
pass in39.757s. These controls do not accept installed behavior. The package
source closure covers the missing sealed helper unit and known legacy metadata;
its private validation and actual package acceptance remain separate.

### Retained clipboard read boundary

The preceding exact source/binaries passed Release34, Debug35 with ASan/UBSan/
LSan, strict static31 and native344/344 (receipt2311c4e9,21:20:39UTC). Actual
installed78 stopped after34PASS on the first GTK TextView literalTab; source
text and clipboard stayed intact and original enabled status was independently
restored. Its observer timeout/cleanup failure remains recorded.

Real-X causal controls expose two shared ClipboardManager read fences that
mistake ordinary peer requests for incomplete identity history: origin capture
and post-notify header retirement. Only those fences finish bounded queued
history; original deadlines, saturation/ABA/owner checks and grabbed-publication
Yield remained unchanged at that snapshot. Its source/native acceptance completed: Release34,
Debug35 with ASan/UBSan/LSan, strict static31 and344/344 all pass. Actual
receipt753ffc58 ended2026-10-04 22:19:22UTC; independent audit83acd9a2 verifies
all165 frozen inputs,64 ELF identities and13 actual packet logs. Zero failures,
errors/skips/unfinished cases/drift/OOM events. Installed78 acceptance is open.
Evidence and the finite delivery route remain linked in the checkbox plan;
earlier passes do not accept changed binaries or the final package.

### Retained preparation-admission checkpoint

Preparation admission now yields only to actual
observation at the existing AccessibleContext single-flight owner. Explicit
private preparation intent does not carry cached-origin authority. Causal old
code refuses the working read; corrected actual helper controls pass2/2,
including retired-peer close and a late provider reply. Affected GTK10/10 and
Chromium5/5 pass. Current Release34/34 passes76.39s; ASan/UBSan Debug35/35 with
LSan enabled passes85.00s. Current daemon is `a98e4f7a`, helper `a03233d3`;
installed guest bytes are unchanged.

The preceding final339 completed336PASS/3FAIL/zero errors or skips. Its failures
and all preceding evidence remain retained. Strict31 static passes with all87
fresh input pins matching; additional Debug GTK clipboard1/1 passes under its
existing no-LSan fixture property. The complete341 inventory, retaining all339
selectors plus two admission controls, passes341/341 with zero failures/errors/
skips/unstarted/unfinished/retries/waivers and all165 source/Release input pins
matching. Independent source/SRP/terminal review accepts the P1 technical gate;
receipt `c93e79d1` ended2026-10-04 17:11:29UTC, no OOM/PID-limit events.
Installed78, final-DEB lifecycle/soak, host and release remain unaccepted.

### Retained effective-status checkpoint

The 2026-10-04 trace-enabled complete336 run
finished335 PASS/one diagnostic-only GTK failure, with zero errors, skips or
unfinished cases. The retained input assertions passed; test-only empty-state
logs evicted an early fallback marker. Its unchanged focused test now passes
after bounding those prints to meaningful word/token state.

A separate causal Chromium test proves acknowledged redundant SET_STATUS1
cleared a successfully captured six-character word origin before Tab. A paired
primary/secondary configuration acknowledgement isolates the equivalent GTK
failure. Both owning transitions now reset only for an actual effective value
change (secondary forced initialization still resets). Publication/durability,
configuration and active-macro cancellation remain unchanged. Real Chromium
controls pass5/5; GTK controls pass8/8, including actual OFF and fresh ON recovery.
That snapshot passed Release34/34 and ASan/UBSan Debug35/35. Its subsequently
completed expanded339 native/legacy inventory failed three cases as recorded
above; those results do not qualify the changed preparation-priority candidate.
The earlier cold-Tab failure is not attributed to this cause without evidence.
Final current-source/native acceptance and installed product acceptance remain
open. The earlier source results below are historical, not a PASS for a changed
candidate; P2/P3/host publication have not been executed in this checkpoint.

The earlier local source acceptance is in
[s2-g13-final-owner-integration.md](platform/evidence/autonomous-20261002/s2-g13-final-owner-integration.md):
GTK/VTE221/221 and Chromium115/115, Release34/34, Debug35/35 with ASan/UBSan
and the LSan negative control, strict static31/31. GTK evidence is reused only
because its fixture and product bytes still match; Chromium was revalidated
after clarifying the hundred-operation fault antecedent. Earlier331/336 and
335/336 failures remain separate evidence. No production guard was weakened.

The earlier G13 integrated owner checkpoint is in
[s2-g13-integrated-owner-checkpoint.md](platform/evidence/autonomous-20261002/s2-g13-integrated-owner-checkpoint.md).
The current native GTK/VTE feature packet passes26/26, including terminal word
conversion, repeated transformations, inverse undo and exact following PTY
input. Partial Backspace and consecutive words preserve their captured child
origin. Raw clipboard tests pass, including INCR, full portable payload recovery,
peer death and lost Set/Release acknowledgements with foreign-copy controls.
Quiet waiting no longer destroys the publisher through a remote heartbeat.
Peer responses consume both checked cookies, and known-dead requestors receive
no redundant cleanup write. Independent final source SRP review passes the
current ClipboardManager; this does not accept S2/S3 or the complete product.

The private GNOME endpoint and pre-enabled private accessibility bus are fixture
conditions, not cold installed-provider proof. Package fresh install, exact2.8.5
upgrade/rollback, installed tray/audio and1800s/200-operation sustained operation
remain open. The host Punto package and graphical session have not been changed.
Only the missing build prerequisite libxcb-res0-dev1.15-1ubuntu2 was installed;
its res.h is identical to the private validation SDK. These downstream gates
precede reversible host installation and publication.

The retained platform prerequisites now pass: a fresh consumer build completed
16/16 compilation and 12/12 link/ELF checks; final integrated Q is 40/40 and
its loaded-object classifier controls are 9/9. All 348 retained library hashes
and 13124 recorded modes are unchanged. These are transport checks, not product
acceptance.

The ordinary GTK build no longer requires patched GDK/Xlib/XCB recipient APIs.
Mixed native identities and missing/stale native ABI are rejected; real matched
native modules and stock-mode recovery load successfully. Build checks are 5/5
without skips on the explicitly selected native SDK. The ordinary Release
build completed 218/218 steps. Isolated contracts are 32/32 after correcting
the hello test fixture to supply its mandatory nonzero stream epoch; production
validation was not weakened.

The user chose no mandatory browser extension or GTK editor adapter. The
ordinary GUI path now selects and confirms a fresh application-native PRIMARY
range, then uses bounded X11 replay. Manual current-word commands no longer
derive their range from window-scoped typing history. Automatic correction and
undo still require exact expected text; held automatic Tab pastes one complete
plaintext payload after desktop/range preflight, then releases the original
Tab once. Generic commands retain bounded direct replay. This is an X11
compatibility capability, not an atomic editor-acknowledged CAS transaction.
The existing GNOME desktop-source extension/bridge is a separate layout owner
and has not been removed or restarted.

The immutable G11 full-matrix checkpoint is GTK 192/196 and Chromium 111/112,
without skips; Release 34/34, Debug 35/35 and all 30 static units pass on those
recorded bytes. G12 corrects three shared owners: expired cleanup after a healthy
read timeout, foreground fixture IPC starving the real GTK PRIMARY provider,
and conditional clipboard publication/restoration overwriting newer ownership.
The latter uses one bounded same-connection server grab, exact owner/epoch/history
validation and checked ungrab before request serving; uncertain acknowledgements
disconnect without blind retry. NONE snapshots use the same guarded path.

Fresh G12 evidence is full CM contract PASS, post-review ownership controls 9/9,
real Chromium product controls 5/5 and GTK 8/8. Earlier cleanup controls 16/16 and
foreground IPC controls 5/5 have their own source/command lineage. Independent
causal/source/SRP reviews pass. These focused results do not replace full-matrix
acceptance. The [G12 checkpoint](platform/evidence/autonomous-20261002/s2-g12-owner-checkpoint.md)
records intermediate fixture/build failures separately from causal product REDs.
True reader-disconnect preparation recovery, VTE unfinished-input correction,
rich/custom automatic Tab and literal-Tab undo/learning/sound remain unresolved;
S2/S3 and release gates are not accepted.

The G12 current source gates pass separately: Release contracts 34/34, Debug
35/35 with the LSan negative control, and all 30 exact-CI static units. Its frozen
source/runtime identities are recorded in
[the G12 manifest](platform/evidence/autonomous-20261002/s3-source-manifest-g12.json).
The linked graph is blocked at the remaining product contract, not at C2C or
tool availability. The host is still `punto-switcher 2.8.13`, with active udevmon.

The historical 188/191 GTK and 111/111 Chromium snapshots are superseded, not
current acceptance. The complete G9 GTK run is 191/193: original positive VTE
and intermittent cleanup recovery failures remain recorded. G9 passes ordinary
Release 34/34, Debug 35/35 and all 30 exact-CI static units. Its source/binary
identity is preserved in the [G9 manifest](platform/evidence/autonomous-20261002/s3-source-manifest-g9.json).
Do not add passing isolated repeats to those matrix counts.

G10's causal control uses the original GTK connection to reassert the same
PRIMARY window with a changed server timestamp; a stale Pause witnesses actual
NONE without selecting or writing. Fresh input fails on unchanged G9 runtime.
ClipboardManager now reports explicit rejection separately from healthy
timeout. WordEditor admits fresh range preparation only through the existing
same-owner/connection and focus/session lineage, using the current unreadable
identity as baseline. Old payload is not replay authority. Normal exact manual
retries are retained. Independent BDD/SRP and causal RED reviews pass; rebuilt
focused GTK 6/6, Chromium 7/7 and complete original-UID clipboard 1/1 are GREEN.
The complete G10 matrices, sanitizer/static gates and final audit remain
separate. S2 is not accepted; positive VTE and arbitrary rich/custom automatic
Tab disposition cannot be replaced by safe refusals.

Default CMake/deb sources no longer build, ship or auto-load the GTK editor
module. A separately selected matched-native SDK retains its experimental
adapter. Default/native graph checks pass 5/5; maintainer and dpkg-ownership
fixtures pass 20/20. The latter remove locally modified predecessor-owned
adapter files while preserving a foreign sentinel, not an installed candidate.
The ordinary clang-tidy inventory now selects 30 sources; the explicit native
profile selects 31, including xorg_recipient.cpp with a clean scoped analyzer
check. Inventory controls pass 5/5. G9's complete static PASS is historical for
G10, which changes a shared header and requires its own complete source run.
Source/fixture checks do not establish a candidate archive. Host, guest and
retained platform libraries are untouched.

Full feature, VTE, guest lifecycle, soak and installed-runtime acceptance remain
open. No candidate package, host update, commit or release publication was
performed. The host remains on 2.8.13; GNOME Shell and IBus were not restarted.
Current scenarios and uncertainty limits are in the
[extensionless contract](platform/evidence/autonomous-20261002/s2-extensionless-contract.md);
the [owner audit packet](platform/evidence/autonomous-20261002/s2-extensionless-review-packet.md)
keeps exact source/binary evidence separate from deployment readiness. Earlier
adapter-path REDs remain in the historical
[S2 checkpoint](platform/evidence/autonomous-20261002/s2-checkpoint.md).

## Historical integrated G0–G5 checkpoint (R52)

The single feature/owner/oracle inventory is locally complete (G0 only).
G1/GTK is RED: a source-aligned isolated two-Entry test focuses the second
field without a delimiter, types its first word, and gets a safe
`editor_prepare/range_unavailable` rejection instead of its first correction.
The earlier `selection_confirmation` result came from a stale driver without
the GTK adapter and is not evidence for the current CompareApply path. The
earlier installed Entry-to-TextView direct-switch failure is independent
real-input evidence of the same missing editor-context boundary. See
`protocol/c2c-bfdc-i7-r52-evidence/g1-gtk-context-red.md`.

The R52 editor-origin amendment is stopped at G1, before a production patch.
The present daemon-to-adapter request can overtake the key frame it describes:
the adapter has no acknowledgement that its widget processed that frame before
returning a snapshot. Adding editor identity to an immediate socket response
would not establish that ordering. The separate reparent test is retained but
not run as a substitute for the two-precreated-field RED. See
`protocol/c2c-bfdc-i7-r52-evidence/g1-origin-order-blocked.md` for the exact
source/binary lineage and counter-schedule. This is a protocol design blocker,
not an approval to install, publish or reduce the documented feature scope.

G3 has isolated transaction and exact-2.8.5 preflight checks, but no new
guest upgrade or input-verified rollback on the current sources. The working
guest remains on 2.8.5; the host is untouched. In particular, timeout is
now an unresolved operation, not a reason to terminate dpkg and start a new
transaction. The historical deployment-wrapper description below records an
older implementation and must not be read as this R52 contract. See
`protocol/c2c-bfdc-i7-r52-evidence/g3-delivery-preparation.md`.

Chromium/VTE product owners, the remaining documented functions, a joint
immutable candidate, installed acceptance and soak remain open. No source,
package or release PASS follows from G0 or the isolated G3 checks.

Version 2.8.12 was marked prerelease after live GNOME validation exposed valid
layout transitions taking longer than its 300 ms macro deadline. It remains
available as historical evidence, but is not the stable release candidate.

## 2.8.14 product verification matrix

This matrix binds the documented capabilities to their current production
owners and required real test seams. Status is the autonomous 2026-10-03
checkpoint, not a claim of a final source candidate; publication and installed
runtime acceptance remain separate. Historical R52 evidence below is not
attributed to the current extensionless source.

| Capability | Production owner | Verification seam | Candidate result / limit |
| --- | --- | --- | --- |
| Manual word layout and case | `EventLoop` + `WordEditor` | Native GTK and Chromium editor text, repeated `Pause`, actual source versus desktop source, next input | PARTIAL: current GTK/Chromium cross-field, range, direction and recovery checks pass; installed acceptance and terminal word owner remain open |
| Automatic correction | analyzer/sequencer + `EventLoop` + `WordEditor` | Native GTK/Chromium consecutive corrections, punctuation, Tab and younger-tail cases | PARTIAL: G10 GTK 194/195, original positive VTE only; nested-delete and cleanup recovery pass. Rich/custom automatic Tab and successful-Tab consumers remain open |
| Selection layout, case and transliteration | `WordEditor` guarded replay/plaintext paste | Native GTK, Chromium and VTE selection cases | PARTIAL: current range/clipboard/case/translit checks pass, including bounded printable single-line VTE selection; terminal current-word/auto/undo remain unaccepted |
| Desktop layout transition | `DesktopLayoutClient` + session bridge + GNOME extension | Desktop-source fixture, helper lifecycle, lock/modifier lifecycle and negative XKB-only control | PARTIAL: isolated contracts and read-only host probe; installed bridge transition unaccepted |
| Undo and learning | `EventLoop` + `UndoDetector` | Immediate/expired undo, invalidation, exclusion persistence and reset across restart | PARTIAL: ordinary path covered; successful held Tab bypasses registration and requires a same-editor identity boundary; installed acceptance open |
| Correction sound | `EventLoop` consumer + `SoundManager` async launcher | Real invocation on layout-changing correction, disabled/no-change/refused negatives; launcher contracts | PARTIAL: launcher/settings owners covered, but held Tab skips EventLoop sound consumer |
| Clipboard ownership and recovery | `ClipboardManager` transport + `WordEditor` operation lifetime | Late receipt, foreign copy, publication/arm/context/chord refusal and fresh recovery | PARTIAL: current full browser/local boundaries pass; installed gates remain open |
| Terminal safety | `WordEditor` terminal policy | Native VTE/PTY word and selection cases, scrollback and stalled-paste recovery | PARTIAL: bounded single-line selection path passes real PTY checks; terminal word/auto/undo still reject at `terminal_range_unverified`, not removed from scope |
| Runtime control and reload | IPC + CLI + tray | Native IPC/tray contracts, config races and CLI checks | PARTIAL: local contracts; installed lifecycle gate open |
| Service and package lifecycle | systemd/udevmon scripts + Debian package | Isolated package build/install/direct 2.8.5 upgrade/rollback, ShellCheck | PARTIAL: prior full package contract 949/949; disposable guest upgraded from 2.8.5 and rolled back with input restored, but conffile prompt and manual job migration block unattended delivery |
| Diagnostics | runtime health + `STATS` + journald | Native health contracts and rejection-stage assertions | PARTIAL: local contracts; installed diagnostic acceptance open |

## R52 installed-guest checkpoint, 2026-09-26

The prior full package contract finished 949/949 with no skips. Its complete
log is in `protocol/c2c-bfdc-i7-r52-evidence/package-contract-949.log`.
That temporary package no longer exists; its PASS is not attributed to the
later installed candidate. The guest-tested package has SHA-256
`ca2714e87a4854def05162059ed503285c330179d36ac53ad6c74b72942ecd03`.
The exact archived 2.8.5 package has SHA-256
`80c977e4b195f73bdbf89c9e2ba8b1c4162a6c318ea74142fedd6010d222ebd4`.

In the disposable `punto-r23-native` VM, package installation placed the GTK
module and descriptor correctly but the running GNOME XSettings initially
continued publishing only `canberra-gtk-module`. GNOME 46's directory monitor
compares whole-second mtimes; a second metadata update made the Punto module
visible without restarting GNOME Shell or IBus. A focused regression failed
against the original candidate and passed 97/97 after `postinst` ensured a
later directory mtime. The fixed package was tested through an actual 2.8.5
upgrade; XSettings then published `punto-gtk-editor-adapter` automatically.

The 2.8.5 `prerm` stopped `udevmon` as expected. Plain noninteractive
`dpkg -i` then stopped at the legacy tray-autostart conffile prompt and left
the new package unpacked but unconfigured, with input interception inactive.
After inspecting that terminal transaction, the guest was configured with
`--force-confdef --force-confold`, preserving the config. Only the exact
archived Punto job and tray file were migrated using verified hashes; no
foreign interception job was replaced. Explicit activation produced one
`interception → /usr/bin/punto-daemon → uinput` chain. The observed outage
from upgrade start to activation was 2 minutes 14 seconds, including diagnosis
and controlled recovery, not package-script execution time.

A fresh GTK Entry launched without `GTK_MODULES`, `GTK_PATH`, or `LD_PRELOAD`
mapped the installed module and exposed an abstract socket whose peer PID/UID
matched that editor process. Physical input `ghbdtn` became `привет` on Pause,
with RU selected, clipboard unchanged, and all modifiers released. A direct
fresh TextView attempt after closing that Entry kept `руддщ` unchanged:
`editor_prepare` rejected at `range_unavailable`. With a physical Enter
separator, the same TextView path converted `руддщ` to `hello` and selected EN.
This isolates a cross-editor word-buffer boundary defect; the direct-switch
case remains a product blocker.

Rollback to the exact 2.8.5 package completed through real `dpkg` and its
`postinst`. The old job, tray file and config matched their archived hashes;
one `/usr/local/bin/punto-daemon` pipeline and a physical `f` into GTK Entry
confirmed restored input. GNOME Shell and IBus were not restarted. The host
package and service were not changed. The VM has a verified cold-image
checkpoint; `protocol/c2c-bfdc-i7-r52-evidence/guest-recovery-runbook.md`
records the controls. This guest slice does not clear Chromium/VTE, the GTK
focus defect, source gates, host acceptance, soak, or release review.

## Desktop-owned layout transition contract

Persistent desktop-source transitions are requested through the authenticated
`DesktopLayoutClient` → `punto-session-bridge` → GNOME Shell extension chain
and are accepted only after the target source and group are observed with the
same session, focus, pointer position and lock modifiers. The configured
`hotkey` only identifies physical user-driven layout events to EventLoop; Punto
does not synthesize that chord in production. During mapped GUI
word or selection replay and paste, WordEditor may lock an internal XKB group
temporarily.
It restores the exact source group within 100 ms before requesting the final
desktop transition. Restoration is allowed only in the original session,
focus, pointer and lock context, and only while XKB is in an executor-owned
temporary or source group. Ordinary work uses a 300 ms phase budget.
A required desktop transition gets up to 1 s, then the ordinary budget is
renewed; a 3.5 s hard deadline bounds the complete operation. A synchronous
client must hold the target group for 5 ms. After an observed focus return or
an authenticated physical desktop source transition, the settle interval is
250 ms. The
original GUI selection receipt must remain stable for 30 ms; equal text in
the same client is not accepted as a recreated range. Word paths confirm the
exact prepared suffix, perform the desktop transition, and revalidate that same
receipt before replay. GUI selection paths replace the confirmed range before
any desktop transition can disturb it. If a later transition fails after an editor action was
actually queued, the result is a partial failure rather than a successful
dispatch; no blind undo or compensating toggle is attempted. The desktop owns
and applies its configured shortcut; Punto only authenticates the resulting
physical source transition and never synthesizes that chord.

GUI selection conversion does not flatten the clipboard. If the replacement
can be mapped to supported XKB strokes, WordEditor confirms the original exact
selection, replays the replacement directly, and leaves the clipboard owner and
targets untouched. An unmappable replacement fails before destructive mutation.
Terminal selection keeps the bounded clipboard/paste protocol because direct
replay would insert at the prompt rather than replace scrollback selection;
completion depends on the paste receipt and bounded client settlement, not on
PRIMARY changing identity.

The shipped default is `leftctrl+grave`, restored as the physical-event
contract. The setting remains a binding identity observed by Punto, not a
binding managed or synthesized by Punto. Mutter/GNOME 46 names the key above Tab
`Above_Tab`, so its accelerator is `<Control>Above_Tab` without a trailing
angle bracket. GNOME 46 rejects `<Control>Cyrillic_io` as invalid. Binding
readback is insufficient: release
acceptance requires two consecutive physical transitions in opposite
directions with the same persistent desktop source and XKB state.

EventLoop uses the immutable hotkey snapshot to recognize the physical user
transition associated with the admitted word candidate, including delayed
automatic correction and undo. That authenticated transition clears pending
word/history state but keeps only a completed Chromium selection receipt,
allowing the next correction after the desktop changes its active source. Other
control chords and all runtime, configuration, session and focus resets continue
to invalidate that receipt.

Native Xvfb tests use a desktop-shortcut stand-in whose source index changes
from its own state and is never derived from XKB at activation time. A negative
control changes only XKB and proves the desktop source remains unchanged.
Captured, captured-without-switch, absent, delayed, activation-limit and
lock-modifier variants cover safe preflight rejection, recovery without restart
or reselection, and honest post-paste partial failure. Chromium checks fresh DOM
serials, text, caret, group, source index, activation count, exact modifier
sides/lifecycle and recovery without stuck synthetic keys. GTK/VTE additionally
prove clipboard preservation and rejection before terminal Backspace.

Iteration 3 separates a safe desktop-shortcut timeout from a detected context
violation. A retained Chromium receipt survives only the former: focus, pointer,
lock, session or non-deadline observation failures invalidate it permanently.
The post-arm/pre-dispatch paste path now cancels its receipt before the
generation-checked clipboard rollback. Deterministic fixture barriers exercise
all three live context changes and the exact post-arm boundary. Negative browser
checks take their serial after the rejection, delayed activation or release and
then require a later DOM snapshot.

Iteration 4 closes the remaining transport exits after retained receipt
admission. A failed keymap reply or checked XTEST layout request, cancellation,
or any unconfirmed context now invalidates the receipt. The only preserving exit
is a fully accepted desktop layout chord followed by repeated observation of the
unchanged group and context until the shared macro deadline. Driver-only fault
injection proves both protocol-error paths reject a later retry without a second
dispatch, while the safe timeout control still recovers to a second dispatch.
The post-arm GTK regression now observes the direct XTEST paste key in the
editor itself, with the successful recovery as its positive control.

Iteration 5 separates ownership of the admitted prior receipt from the receipt
created by the current successful dispatch. The prior receipt stays local and
returns to object state only for the explicitly safe pre-mutation desktop
activation timeout; mutation permanently consumes it. A newly prepared receipt
then has its own post-dispatch lifecycle. Exhausting the shared budget or a
new-input interruption before a further context check or in its lower wait ends
settlement without revoking that receipt. Pointer, focus, lock, session and
observation failures still revoke it. Native Chromium tests cover four
consecutive Pause corrections, both deadline boundaries and each context change
after both a first and retained correction. The ordinary repeated-manual and
consecutive-automatic scenarios also passed ten consecutive focused runs after
the final post-dispatch classification change.

The final sanitizer pass also exposed an intermittent completed paste whose
receipt was not confirmed: `pump_events()` had reused its two-millisecond
aggregate polling deadline for the checked property-and-notify transfer. A
selection request now receives ClipboardManager's existing ten-millisecond
operation budget and ends that pump cycle, so the bound is not multiplied by
the event count. The original failing literal-Tab scenario subsequently passed
20 consecutive Debug sanitizer runs together with the late-receipt,
foreign-copy and post-arm rejection contracts. No timeout value was increased.

Final iteration-5 evidence was produced from one unchanged source snapshot.
Release passed all 26 CTest targets, including 103 GTK/VTE and 41
Chromium/browser-lifecycle cases. Debug passed the 25 non-clipboard-GTK targets
with ASan, UBSan and LeakSanitizer enabled; the separate clipboard-GTK target
passed with the narrow external GTK cache exception documented below.
CI-equivalent clang-tidy, Python fixture compilation, clang-format and
whitespace checks also passed. The final binaries from that snapshot have these
SHA256 fingerprints:

| Artifact | SHA256 |
| --- | --- |
| Release daemon | `b2d1b94c7067b003058e61c0d9af3dcdfff6018a22f18b805aa8c0f3874860d0` |
| Release EventLoop driver | `bce34967aad7dfb8349c651d10778607cfcd197440d49c8e76eb41d4429ae558` |
| Debug daemon | `1f2ed73c897f453b6be47e364dec0eaf3fa2483db60bf58fb044ad7b5b271fbb` |
| Debug EventLoop driver | `f47076051632b43c76b20b234df8325aadbda60e08c3ac56fe0ef3eb25b7d010` |

Iteration 7 closes the GNOME live-layout blocker found after v2.8.12. A desktop
transition may temporarily move focus or leave Punto's own chord visible in
XKB state. WordEditor accepts only those exact transients, waits for the client
to settle, and rejects any foreign key, pointer, lock, session or unexpected
group change before further mutation. Word correction was verified through the
real evdev -> interception -> candidate -> uinput path in both layout
directions. The next physical key used the final desktop group.

Iteration 8 removes range reconstruction after desktop transitions. A GUI word
now stabilizes its exact prepared receipt, performs the desktop transition, and
revalidates the same receipt before replay; a disturbed or adjacent same-text
range is rejected without replacement. GUI selection conversion confirms the
original selection, performs direct mapped replay, restores any temporary
internal XKB group under the original context, and only then requests the final
desktop source. Mixed-layout replay uses the same bounded cleanup, and completed
newer words retain their observed final layout without desktop oscillation.
Adjacent duplicate, middle-field suffix, desktop disturbance, XTEST failure,
cancellation and deadline regressions cover the unsafe boundaries. The X11
session/config race fixture now observes a driver-only post-commit marker
instead of parsing concurrently written diagnostic lines. Chromium clipboard verification compares the owner,
complete target set and checked rich-target bytes. Its retained exact receipt
admits the next word correction and undo without clearing PRIMARY. Terminal
completion uses the paste receipt and bounded client settlement rather than a
PRIMARY identity change.

Final source evidence for iteration 8: Release 26/26 in 213.95 seconds with
107 GTK/VTE and 56 Chromium cases; Debug 26/26 in 230.50 seconds with leak
detection plus the separate GTK clipboard target (1/1 in 0.27 seconds);
clang-tidy 23 production translation units; CLI 5315/5315; packaging
817/817; ShellCheck, Python compilation, clang-format and whitespace checks all
pass. The corresponding local build fingerprints are:

| Artifact | SHA256 |
| --- | --- |
| Release daemon | `314a8a45483420677b46970f23a4f4621505b179afb72dc7a3bbeeff494ff3cb` |
| Release EventLoop driver | `0a4985fb2132792226cd55703b6013f3731cba01697a40a54c0f188cd9bef33c` |
| Debug daemon | `cb29911c65a310be46f3d370b8ba2b1a1c355a8ce0ce67ed8f369186c694884c` |
| Debug EventLoop driver | `d6bc947999b908c74f34732ab7b610b4926126735e87f20079d0ff333b2a3ce9` |

Iteration 9 makes temporary internal XKB mutation a single scoped transaction.
The restore obligation is recorded before the checked request can reach the X
server, so an applied request followed by a failed barrier is still observed
and compensated. One scope finalizer covers every paste, direct-selection and
mixed-word exit. It owns a reserved 100 ms slice of the existing 3.5 s hard
deadline and uses freshly observed lease, focus, pointer, lock and group state
after keyboard-idle waiting. Each failed observation stops before the next XCB
request; an uncertain restore write is re-authorized and observed after a
bounded reconnect before success is reported.

Chromium demonstrated that retained PRIMARY cannot by itself prove that a
visible word selection survived the desktop shortcut: a synthetic Right left
PRIMARY unchanged and the old implementation inserted `привет` after `ghbdtn`.
GUI word transitions now subscribe to XI2 raw key events on their bounded XCB
connection and accept only the configured layout chord before receipt
revalidation. Any other key rejects before replay. The standard `xcb-xinput`
build/runtime dependency is declared in CMake, CI, package inventory and the
production ELF contract.

The focused matrix covers paste deadline, cancellation, clipboard ownership
takeover, an applied XKB request with a failed check barrier, cleanup focus and
pointer errors, context change after the idle wait, hard-deadline exhaustion,
and GTK/Chromium selection disturbance. Plain and rich clipboard payloads,
desktop source, XKB group, next physical key and recovery without restart are
asserted at their respective boundaries.

Final iteration-9 evidence: Release 26/26 in 223.80 seconds with 115 GTK/VTE
and 57 Chromium/browser-lifecycle cases; Debug ASan/UBSan/LSan 27/27 in 243.61
seconds; clang-tidy 23 production translation units; CLI 5315/5315; packaging
825/825; ShellCheck, Python compilation, clang-format and whitespace checks all
pass. The binaries from this source snapshot have these SHA256 fingerprints:

| Artifact | SHA256 |
| --- | --- |
| Release daemon | `c4855c3bcbadc41335fb43ece40fceda1c08d50624c4d9f5105f352e36bc8dfd` |
| Release EventLoop driver | `6ad6f30ed4f21464774c1e62e00ec7d0d8d3b0521b7bd6b3eba6c4584f603a97` |
| Debug daemon | `4ee6c2f87169ef0098e4264752fbc957a69bb0abfa3c9b9d05f9a666789a1a29` |
| Debug EventLoop driver | `0055b93d281e0f5ac5430fe597dac9de185cc7591de2f6e484aba34e090773ed` |

Iteration 10 makes the reserved cleanup interval an enforced boundary after the
first internal XKB mutation. Every later phase renewal is capped at that
boundary; if replay, context observation or client settlement reaches it,
ordinary work stops and restoration alone may use the remaining part of the
original 3.5-second deadline. The cancellation regression now proves mailbox
admission before releasing the held paste phase instead of succeeding through
the hold timeout.

Reconnect cleanup no longer leaves connection-owned setup metadata in the
continuation path. The keycode range is copied before mutation, while a
reconnected continuation rechecks XTEST and recreates its XI2 raw-key
subscription before any final desktop transition. A checked restore error whose
write was applied by X is covered through reconnect, final desktop activation,
the next physical key and ASan/UBSan/LSan.

XI2 evidence collection is fail-closed and bounded independently of the outer
macro loop: one observation may consume at most 256 queued events and 5 ms.
A foreign raw key exits immediately; count exhaustion, time exhaustion and a
connection error reject before replay. Driver-only regressions distinguish all
three results and retain the existing Chromium Right disturbance and successful
own-chord controls.

Final iteration-10 evidence: Release 26/26 in 222.26 seconds with 120 GTK/VTE
and 57 Chromium/browser-lifecycle cases; Debug ASan/UBSan/LSan 27/27 in 243.36
seconds. Clang-tidy covered all 23 production translation units; CLI passed
5315/5315 checks and packaging passed 825/825. ShellCheck, Python compilation,
changed-file clang-format and whitespace checks passed. The binaries from this
source snapshot have these SHA256 fingerprints:

| Artifact | SHA256 |
| --- | --- |
| Release daemon | `c4cd62a91c5730b57594aff5b8d9a04007640c9007cedba09ab7dd27c9aa3c03` |
| Release EventLoop driver | `58779f58110468fa2815a9bb8db06d95fcbc4e1dfa838776033b487307437ae7` |
| Debug daemon | `d61983e30153cf867c5189603a9de701b90a905a1f8a14586ed3c7954eec1b20` |
| Debug EventLoop driver | `88ed6331ffff6d45d91d8998977ed367ebcc55148fa252018f5235fe417a2249` |

Iteration 11 closes the remaining XInput2 initialization failure path. Every
barrier and XIQueryVersion result is checked before the next use of the bounded
XCB connection. A timeout, protocol error or transport failure therefore
rejects the operation without calling `xcb_get_setup` or another XCB API through
a connection closed by that result. The same fail-closed sequence runs during
initial word admission and after cleanup reconnect.

Driver-only faults target `Initial/AfterRestore` and `Barrier/Version`
independently. The regressions assert exact fault consumption, bounded return,
editor selection and caret, PRIMARY and CLIPBOARD, XKB group, desktop source,
dispatch count, IPC availability and a following successful correction without
daemon restart. A separate marker is emitted only when the reopened connection
actually enters XInput2 preparation. The unsafe `xcb_get_setup(nullptr)` oracle
is fail-fast so it cannot hide an unrelated null-connection defect.

Iteration-11 test evidence: Release 26/26 in 223.39 seconds with 125 GTK/VTE
and 57 Chromium/browser-lifecycle cases; Debug ASan/UBSan/LSan 27/27 in 231.65
seconds with the same GTK/Chromium counts. Clang-tidy covered all 23 production
translation units; CLI passed 5315/5315 checks and packaging passed 825/825.
ShellCheck, Python compilation, changed-file clang-format and whitespace checks
passed. The fingerprints recorded at that checkpoint were later found to mix
stale production daemons with freshly rebuilt EventLoop drivers and are retained
only to explain the validation gap:

| Artifact | SHA256 |
| --- | --- |
| Release daemon | `c4cd62a91c5730b57594aff5b8d9a04007640c9007cedba09ab7dd27c9aa3c03` |
| Release EventLoop driver | `2e1d7059de77913e1c229342571b3f90d0b2da48645380719910987dc1319288` |
| Debug daemon | `d61983e30153cf867c5189603a9de701b90a905a1f8a14586ed3c7954eec1b20` |
| Debug EventLoop driver | `0b5c864a74066fa0b8cdcba79fccee05dd460f70fe5465143820932788528837` |

Iteration 12 closes that artifact-provenance gap without changing production or
test logic. Both configurations were generated in new repo-local build trees:

```sh
cmake -S cpp -B cpp/build-i12-release-clean \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build cpp/build-i12-release-clean --verbose -j4

cmake -S cpp -B cpp/build-i12-debug-clean \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build cpp/build-i12-debug-clean --verbose -j4
```

The verbose logs show `word_editor.cpp` compiled separately into both `punto`
and `punto-event-loop-e2e-driver`, followed by linkage of both targets. Release
CTest passed 26/26 in 224.80 seconds, including 125 GTK/VTE and 57
Chromium/browser-lifecycle cases. Debug passed 26/26 in 228.27 seconds with
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`; the separately configured
GTK clipboard contract passed 1/1 with LeakSanitizer disabled, for 27/27 total.
The source and all four artifact hashes were unchanged after validation:

| Artifact | SHA256 |
| --- | --- |
| `cpp/src/word_editor.cpp` | `7224d0c29edcfa05ce8a1f5d0f8af9613820f89cbada21dcd78abe3900020407` |
| Release daemon | `ede79e96de54885368c7a8e61dfdd431d703a33121caee7cdec077e829f5a789` |
| Release EventLoop driver | `2e1d7059de77913e1c229342571b3f90d0b2da48645380719910987dc1319288` |
| Debug daemon | `4ab3b194b35420aa677570c72627b6ded3456386292b8146ec22ff1dd76f7676` |
| Debug EventLoop driver | `c11781a14244347585269f5936192ca89e6ef76c71d4dd50652e6e39b20b138a` |

The clean Release daemon differs from the stale iteration-11 daemon, confirming
that the earlier evidence omitted a production-target rebuild. Debug binaries
also include their build-directory paths in debug information, so only hashes
from the canonical repo-local validation tree above identify the reviewed
artifacts.

## Runtime stability and learning control (2026-09-07, 2.8.11)

Session discovery and keyboard observation now use independent bounded workers.
A slow periodic discovery therefore cannot occupy the lane needed to validate a
Pause action, and a transient first discovery failure no longer revokes the
last known-good write lease before the retry sequence finishes. Tray reads and
mutations likewise have independent in-flight state, so periodic polling cannot
temporarily disable a confirmed control.

CLI start/restart require operational X11, analysis and input health plus the
X11 mutation backend. Tray keeps a confirmed control-plane toggle available
during a temporary data-plane degradation and exposes that state separately.
The last known-good configuration stays operational after a failed reload.
`CLEAR_EXCLUSIONS` / `punto reset-learning` provides an explicit durable reset;
its receipt is fenced by a process-lifetime `daemon_epoch`, so primary failover
cannot acknowledge another daemon's local persistence generation. Native GTK E2E
covers correction while discovery is deliberately blocked and correction after
clearing the exclusion store.

The generic X11 editor boundary remains fail-closed. In particular, an
unattributed stale PRIMARY from another field of the same Chromium client can
still block an edit; relaxing this check collapses a real newer selection and
is therefore not shipped as a workaround.

All sections below this 2.8.11 checkpoint are append-only historical evidence.
Their readiness or blocker verdicts apply only to the version and date named in
that section; they do not override the current contract above and in README.

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
The initial implementation changed five existing files by 160 lines, including
13 production lines; no file or parallel state owner was added.

CI 33998951691 passed Release, sanitizers and package gates but clang-tidy-18
reported ten unchecked optional accesses in the existing guarded PRIMARY loop.
The same diagnostics reproduced locally in `/tmp/punto-key-release-tidy-red.log`.
Isolating only the initial fence in a synchronous local bool lambda made the
unchanged optional guards analyzable: `/tmp/punto-key-release-tidy-green.log`.
The call position, query/context/wait order, deadline, cancellation and rejection
stage are unchanged; no warning suppression or optional access was modified.
Existing passing key-release/editor cases provide the refactor characterization.
BDD critic, separate invariant auditor and pre-SRP review passed. The lambda is
a subordinate mechanical unit of WordEditor, not a second policy owner. The
wrapper adds three lines for static verifiability, with no new state or API.
Final source/SRP audit passed. The exact CI clang-tidy profile on the changed
translation unit passed in `/tmp/punto-key-release-tidy-full-profile.log`.
Unchanged GTK/VTE 91, Chrome 9 and browser lifecycle 3 cases passed again in
both builds: `/tmp/punto-tidy-refactor-release-e2e.log` and
`/tmp/punto-tidy-refactor-debug-e2e.log`. Other source gates are covered by the
preceding runtime CI and repeated in the next exact-commit full CI; no additional
behavior change or test weakening was needed.

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

## Iteration 13: restored grave default and live desktop blocker

The compiled default, shipped YAML, SettingsData projection and extracted DEB
now agree on `leftctrl+grave`. Explicit `space` remains a supported configured
value. The Chromium stale-PRIMARY recovery helper now sends the committed
desktop chord instead of hard-coding Ctrl+Space; its RED reached the intended
post-fault branch with evdev 29/57, then the focused GREEN passed with the
reloaded LeftAlt+Backslash chord. The strengthened physical-shortcut regression
also confirms exact activation deltas, released keys, the same daemon PID and
the next corrected character.

Fresh Release CTest passed 26/26 in 229.04 seconds: GTK/VTE 125 cases and
Chromium 58. Fresh Debug passed 27/27 in 245.64 seconds with ASan, UBSan and
LeakSanitizer enabled. CLI passed 5315/5315, packaging 829/829, and the exact CI
clang-tidy profile passed all 23 production translation units. ShellCheck,
Python compilation, clang-format and whitespace checks passed. Binary hashes:

| Artifact | SHA256 |
| --- | --- |
| Release daemon | `c83feb9c5b0b5e6f463489cda0b9d28a921b25c39c58dbaab1b8fe7ae730fe3a` |
| Release EventLoop driver | `268d859f53b2034273f0aa00ed58e5c39154f14dee9aa18ef4fd7621c962a6ad` |
| Debug daemon | `20b1be2c02a7ea22013c7faf136f88f8a628d8488c25deaa928fcf671601d4a3` |
| Debug EventLoop driver | `06d897cdc2f0f66edf1c2049415404ff8bbad22924cbb37e6f85ea2cc3b4441b` |

The transactional host smoke did not pass. GNOME 46 rejected
`<Control>Cyrillic_io` as an invalid accelerator. Its valid physical-key alias
`<Control>Above_Tab` still exposed divergence between the desktop source,
IBus engine and XKB group: one transition could succeed while a later identical
chord left the source unchanged. This reproduces the reported one-shot layout
failure and blocks release/install. The transaction restored the exact original
user config hash `aa4fbc2d8b37ba205101437d62a6e430cff8af058e7047f1df0024198f1cb6a2`,
forward/backward bindings, installed v2.8.12 binary hashes and daemon
PIDs/start times. Reload generation advanced 5 to 6 with `config_pending=0
config_result=ok`; no binary or process was replaced. The desktop source itself
could not be made internally consistent: even an explicit IBus RU→US reset left
`IBus=us`, GNOME `current=0` and XKB `group=1`. That unresolved state is part of
the release blocker rather than a successful rollback claim.

## Iteration 14: GNOME 46 X11 input-source freeze compatibility

Source inspection of GNOME Shell 46.0 and Mutter 46.2 localized the one-shot
failure outside Punto's editor pipeline. `activateInputSource()` froze the X11
keyboard before applying the source and passed `releaseKeyboard` to the
asynchronous IBus engine change. The popup had already dropped its modal grab
when that callback ran, so `releaseKeyboard()` selected the ungrab path while
the synchronous XI2 freeze remained observable. The same failure followed a
menu source change, which excluded Punto's synthesized chord as the owner.

The opt-in extension installed by the package is a narrow backport of the later
GNOME change that removed this X11-only hold/release pair. On GNOME 46 X11 it
shadows `freeze_keyboard` with a no-op and omits only the exact paired
`KeyboardManager.releaseKeyboard` callback when delegating to the original
`setEngine`. It never selects a source or writes IBus/XKB state. Wayland, other
Shell versions, changed method shapes, pre-existing wrappers and stale or
foreign handles fail closed. Disable removes both wrappers only while the
extension still owns them. It performs no unpaired release on enable, so an
already damaged session must be replaced by a fresh login.

The final live test used a clean nested GNOME Shell 46.0/Mutter 46.2 X11
session, one persistent kernel uinput keyboard and the real
`<Control>Above_Tab` accelerator. Fifty consecutive physical transitions ran
without reset or retry. The client observed 50 Control presses, zero delivered
grave presses and 50 following key sentinels in the exact alternating sequence
`Cyrillic_a,f`; the final XKB group was 0. A separate GNOME menu EN-to-RU change
reached group 1 and the next physical sentinel reached the same client.
Disable/re-enable returned the extension to `ACTIVE`; the next physical chord
returned to group 0 and its sentinel was received. The host forward binding was
restored exactly to `['<Control>Above_Tab']` after every transaction. These
isolated results supersede the iteration-13 live blocker; installation into the
user's existing session remains a separate release/runtime gate.

Final source validation passed Release CTest 27/27 in 230.77 seconds, including
GTK/VTE and Chromium E2E; Debug passed 27/27 in 246.15 seconds with ASan, UBSan
and LeakSanitizer, plus the separate GTK clipboard test 1/1. CLI passed
5315/5315, packaging passed 837/837, and the exact CI clang-tidy profile passed
all 23 production translation units. ShellCheck, Python compilation,
clang-format, whitespace checks and the GJS compatibility contract passed.

| Artifact | SHA256 |
| --- | --- |
| Release daemon | `c83feb9c5b0b5e6f463489cda0b9d28a921b25c39c58dbaab1b8fe7ae730fe3a` |
| Release EventLoop driver | `268d859f53b2034273f0aa00ed58e5c39154f14dee9aa18ef4fd7621c962a6ad` |
| Debug daemon | `aba7d0c660fad5930820233a2a342362f91fe65c2c20535337889b37efc5e1c7` |
| Debug EventLoop driver | `efd4400ef7993546163efbf8a07133fbd7969d705ec266380dbc08662d8140e3` |

## Iteration 15: bounded recovery after a collapsed Chromium receipt

The installed 2.8.13 runtime remained alive while manual and automatic word
correction stopped. Journald tied the repeated
`retained_selection_context` and `layout_transition_timeout` rejections to one
daemon PID, excluding a restart or competing-daemon explanation for the
observed failure chain. The liveness loss was inside the editor admission
state: a reset discarded the only trusted receipt for Chromium's retained
PRIMARY, and every later correction then failed closed with no safe way to
establish a new baseline.

WordEditor now demotes that receipt instead of forgetting it. A later word can
cross the untrusted state only with a strictly newer textual-input sequence in
the same focus and X11 session. Function keys and Punto's own hotkey do not
advance this fence. The candidate must still prepare and confirm the exact
current suffix before replacement, so an immediate retry, a foreign focus,
another selection identity, pointer or lock drift, or a changed session remains
non-mutating. Safe failures before text replacement advertise a typed
`AfterFreshInput` retry policy; EventLoop grants an automatic candidate one
retry only after a later completed word. The word id and textual-input fence
prevent tight loops and replay after the budget is consumed.

Native Chromium regressions cover wrong modifier sides, extra modifiers,
layout-send failure followed by 26 rejected attempts and same-process recovery,
automatic retry success and exhaustion, and a late desktop transition followed
by a successful new word. The separate soak performs 100 alternating manual
conversions with three injected layout-shortcut failure/recovery cycles and
read-only `STATS` polling. Native GTK covers the same late-transition recovery
without restarting the daemon.

Final source validation used the current candidate at base HEAD
`8f8e24aefede115754c9ce5fefa5905ab9f43f8a`. Release passed 28/28 CTest
targets in 281.69 seconds, including the 100-conversion soak. Debug passed all
28 non-clipboard-GTK targets in 288.34 seconds with ASan, UBSan and
LeakSanitizer; the separate GTK clipboard target passed 1/1 in 0.28 seconds
with only the documented external GTK leak exception. CLI passed 5315/5315.
The exact CI clang-tidy profile passed all 23 production translation units;
ShellCheck, Python compilation, clang-format and whitespace checks passed.

| Artifact | SHA256 |
| --- | --- |
| Release daemon | `b0dde815e2476c425e3f76c7a2f12c6a8fa93ab4cf82c771e3370afa80cfdaca` |
| Release EventLoop driver | `585eeba562d07997bcb028ce39194563c3d4262ec1dbb3bdecb8e5aefa3beae5` |
| Debug daemon | `fdad492136c8998f4540ecb544f036d75d82fe093059614786bfc0933bfb273c` |
| Debug EventLoop driver | `8ff8cf7ecac9aeba921b421051bbf6b16510181f08c89eb49edcdd508fe09f9c` |

## Iteration 16: identity-bound selection and late-transition convergence

Iteration 15's fresh-input demotion mechanism was removed after independent
review found that later typing in the same Chromium client did not prove that
the retained PRIMARY still described the active word. A reset now invalidates
the receipt. A payload-less collapsed Chromium selection is accepted only while
its XFixes owner, selection timestamp and owner generation remain identical to
the receipt created by Punto; a readable PRIMARY must still match its exact
payload. A new selection in another field is rejected before any editor event,
even when its text matches.

The result of a desktop layout request now records whether the request was not
made, remains unresolved, or its target group was observed. Cleanup performs a
bounded reconciliation of an unresolved request only while session, focus,
pointer, locks, source/target group, released chord, raw input history and the
prepared PRIMARY remain stable. EventLoop consumes a trusted observed group
even when replay is rejected, so transitions immediately before, during and
after cleanup converge without a manual layout toggle. A mismatch between the
desktop source and XKB remains non-mutating.

History rollover no longer finalizes a word that is still awaiting its X11
observation. It evicts only completed or ineligible prefix entries, preventing
every `(max_rollback_words + 1)`th automatic word from being silently lost.
Native regressions cover strict preflight and cleanup oracles, 26 consecutive
failures from zero dispatches followed by recovery, varied automatic words,
100 manual conversions, exact modifier-side rejection, GTK/VTE recovery and
the three deterministic cleanup transition phases.

The installed v2.8.13 runtime was deliberately left unchanged during this
source review. A true host-session candidate recovery test therefore remains a
post-review installation gate; the native tests run in isolated X11 sessions
and do not modify the user's documents or clipboard.

The final Release matrix passed all 33 targets: four fresh-session GTK shards
covering 128 tests passed in 98.59 seconds, and the other 29 targets passed in
239.84 seconds. Debug passed the same four GTK shards in 100.85 seconds and all
29 non-GTK-clipboard targets in 245.02 seconds with ASan, UBSan and
LeakSanitizer; the separate GTK clipboard target passed 1/1 in 0.30 seconds
with only the documented external GTK leak exception. The two GTK cases that
had failed after long-lived shared-fixture runs passed 10/10 focused fresh-
session repetitions before sharding. CLI passed 5315/5315 and packaging passed
837/837. The source manifest was generated after this documentation snapshot.
The exact CI clang-tidy profile passed all 23 production translation units;
ShellCheck, Python compilation, clang-format and whitespace checks passed.

| Artifact | SHA256 |
| --- | --- |
| Release daemon | `0ca79a9f3e31bdc46715f84d793464706fe57b3f8f1137cbdfaf36f51a6e76fa` |
| Release EventLoop driver | `c405601b76fd8135c806747b35dd3b414d6b95e1935752df0d580138841f5ee7` |
| Debug daemon | `b9195f76d8749b4939899f8d0955772c4c00a5a3a9f1c18f6026dfb4fcfeb112` |
| Debug EventLoop driver | `6cd8333c940c3c09e8569c10bd9c37e72a25d49c9a3b65ce44c875953dd50323` |

## Iteration 17: bounded history and complete cleanup authority

`TrackedWord` now separates observation/analysis lifecycle from correction
eligibility. The rollback deque is hard-bounded after every admitted word;
late analysis for an evicted record becomes a no-op, while its independent raw
candidate continues and the newest `(max_rollback_words + 1)`th word remains
correctable. The active raw candidate is non-cancellable; its waiting deque
keeps at most `max_rollback_words` newest candidates, so the total raw bound is
`max_rollback_words + 1`. History and raw-queue counters exist only in the E2E
driver. The production `STATS` response retains its exact 39-token schema. A
process contract starts the real daemon and feeds its response to the real CLI
parser and tray IPC client.

Native GTK regressions run 64 consecutive `NoCorrection` words at limits 1 and
5, then verify a real correction, caret position and the next physical symbol.
Held-X11-observation floods at both limits enqueue 33 words without waiting for
analysis, assert both bounds, release the old observation, and prove that the
newest word still corrects. Late work for an evicted word cannot mutate text.

Cleanup reconciliation now covers the complete Right press/release lifecycle
and the final receipt decision. The raw XI2 fence accepts exactly one ordered
Right pair emitted by this cleanup in addition to the configured layout chord;
an extra Right or any foreign raw key remains unsafe. A deterministic phase-3
barrier after Right proves late target-layout convergence in Chromium without a
manual toggle or daemon restart. Controls at the same barrier reject a foreign
key, an extra Right, pointer or lock drift, focus change, a replacement X11
session and a new same-text selection. Foreign keys are injected over a second
X11 connection before the barrier is released; the test also observes the
resulting Chromium key event. A positive/negative retention oracle proves that
only the unchanged cleanup path stores the prepared selection receipt.

`SelectionRead` also carries the ClipboardManager connection generation.
Identity checks revalidate the connection after each event pump and return
false before issuing XCB work when a pump fails closed. The clipboard contract
injects an X11 error in the initial identity pump, records that no owner query
occurred after disconnect, rejects a receipt across close/open, rejects a
receipt whose connection generation alone is stale, and accepts a new receipt
captured on the replacement connection.

The two GTK cases that previously failed only in an aged X11 fixture now run as
an explicit 20-case alternating sequence inside one sandbox. This continuous
session contract complements the four sharded suites instead of replacing them
with fresh-process repetitions.

The iteration kept causal RED evidence. The real daemon/consumer contract first
rejected the expanded production `STATS` response. A held observation with
`max_rollback_words=1` exposed 32 waiting raw candidates. Replacing the final
cleanup check with the former `context_matches(true)` path made the independent
F8 injection miss `layout_cleanup_foreign_raw_key`. Each focused check passed
after its owning fix, including the generation-only clipboard negative control.

Final candidate validation used the same production source snapshot. Release
passed 34/34 CTest targets in 361.92 seconds; Debug passed 35/35 in 378.08
seconds with the configured ASan, UBSan and LeakSanitizer gates. CLI passed
5315/5315 and package
validation passed 837/837. The exact CI clang-tidy profile passed all 23
production translation units. ShellCheck, Python compilation, clang-format,
whitespace checks and `lintian --fail-on error` passed.

The final test-only cleanup removed a version hardcode, generated the 20-case
sequence from one CMake list, and changed the held old candidate to a real
wrong-layout word. Those exact deltas passed the daemon/shared-session targets
2/2 and raw-bound cases 2/2 in both Release and Debug profiles.

| Candidate artifact | SHA256 |
| --- | --- |
| Release daemon | `7f2e11716faee0a7f5b83f3dbb7c5a6450786db6452ea115fe1b372ac5f558fe` |
| Release EventLoop driver | `8cc68a0ad730bcfe742de8c30657e98be6c5a7835787aa139ce5abe275909438` |
| Debug daemon | `6b0c8c9c7838c38f309ae4735290286dd9b2366b470a11a59da329627ca46b12` |
| Debug EventLoop driver | `667278be43f8f003f6365027e30156078dfbf1aefdce3dc9aabb153fc5d6eee9` |
| Debian package | `29b650a20a87988ffd5d215646bedfd20cb6772e47f853d9a14ba19164f65301` |

The installed v2.8.13 runtime remains unchanged until source, sanitizer,
packaging, independent review and live candidate gates finish. Isolated native
X11 tests are evidence for the candidate, not a substitute for final validation
in the target GNOME session.

## Iteration 18: fenced selection identity and causal cleanup controls

An XFixes receipt is no longer accepted after two merely bounded event pumps.
The owner reply now forms an explicit X11 observation fence. ClipboardManager
then consumes only events already queued before that reply and rejects the
identity as unknown if the 64-event or caller-deadline bound is exhausted. It
also rejects a selection request encountered in that scan. The scan never
waits for later socket traffic and never starts a new WordEditor deadline.

The contract reproduces the old false positive with one real PRIMARY owner,
160 `PropertyNotify` events and a later same-owner XFixes timestamp. On the old
implementation the stale receipt matched. The fixed implementation rejects it,
then accepts a fresh receipt without reconnecting. Existing pump-budget,
close/open and connection-generation cases remain green.

The initial event pump shares the caller's absolute deadline as well. A causal
regression queues a real selection request, stops the X server and gives the
identity check 5 ms while the manager's ordinary request timeout is 100 ms. The
check rejects within the caller bound instead of silently starting a new
request budget.

The unreadable-PRIMARY caller path is covered separately. A first real
Chromium correction creates a retained receipt; a new same-text selection then
has its next PRIMARY conversion made unreadable. The request stops at
`retained_selection_unreadable`, before Shift+Left, replay or clipboard
mutation. Browser text and selection, PRIMARY contents and native dispatch
count remain unchanged.

Receipt retention after cleanup now requires three independent facts after the
Right press/release: the 30 ms settle completed, the actual write lease is
still valid, and the prepared PRIMARY identity still matches at the same
caller deadline. Rejections report `layout_cleanup_settle_timeout`,
`layout_cleanup_session` or `layout_cleanup_selection_identity`; tests require
the intended reason and reject a timeout substitute.

The same-text control creates a new real selection while preserving focus,
window, pointer and locks. The session control revokes the actual held test
lease from an independent driver path, then lets the real replacement session
commit after the macro. Test-only phase time is excluded from the product
cleanup budget but remains subject to the hard macro deadline. Mutation
controls that bypass exactly the identity check or lease revocation retain the
receipt and therefore make their respective negative tests fail, proving that
the tests depend on the intended authority checks.

Release passed all 34 CTest targets in 365.38 seconds. Debug initially reported
34/35 because the existing punctuation-undo/restart case stopped at
`selection_confirmation`; that exact test passed in isolation, its complete
shard passed, and a 10-run isolated soak passed 10/10. A clean non-concurrent
Debug rerun on the final source passed 35/35 in 381.44 seconds with the configured sanitizer
gates. Packaging passed 837/837. The installed v2.8.13 runtime remains
unchanged pending independent source review and the final target-GNOME live
correction gate. CLI passed 5315/5315. The exact CI clang-tidy profile passed
all 23 production translation units; ShellCheck, Python compilation, formatting
of all 11 changed C++ files and `git diff --check` passed. A repository-wide
format probe still finds pre-existing violations in untouched files such as
`asm_utils.hpp` and `undo_detector.cpp`; they are outside this correction and
were not rewritten.

| Iteration 18 candidate | SHA256 |
| --- | --- |
| Code/test diff (`cpp`, `tests`, scripts and packaging metadata) | `a7fd11f7f03525ea2b1e03e4730ac00391eeecc3ac6b9aa7dac9d8c49ca1aa1d` |
| Release daemon | `d74336f6bcf61154e722577f86b42a8e92b913c9fd126509bcafae894235d901` |
| Release EventLoop driver | `90a124b0bb555136991b4ec091c285da54fb0b7729a302062a4b7032880850a1` |
| Debug daemon | `ca1e16a65309155ae23f39683aba15e215d3c8e99a622af16dd755e5f0ee2c52` |
| Debug EventLoop driver | `8068d92b92e50574b0df7a76c49291c2521d5225971de23c03e8ce93a87832e6` |
| Debian candidate | `c91e0bf1cf9cf749866f20e0efaf81dac4368b12a6579f96b27221e7a9ad6170` |

The complete scoped branch diff is 2,337 additions and 183 deletions across 17
files. Product headers and sources account for 499 additions and 85 deletions;
the larger remainder is regression coverage and the restoration record. No new
production file or parallel state owner was added.

## Iteration 19: authenticated GNOME layout authority and final 2.8.14 gates

Programmatic layout correction no longer synthesizes the configured desktop
hotkey. `DesktopLayoutClient` discovers a UID-bound descriptor below
`/run/user/<uid>/punto`, connects to the private peer D-Bus socket and admits an
operation only through a live lease. `punto-session-bridge` is the constrained
user-session proxy; the GNOME Shell 46 X11 extension owns exact `us`/`ru` source
activation, operation idempotency and source/XKB convergence. Missing helpers,
unsupported source maps, variants, locked sessions, stale generations and
transport uncertainty fail closed. The configured `hotkey` remains the identity
of a physical user action that invalidates stale EventLoop context.

Selection confirmation now uses a causal XFixes observation fence and a
separate cleanup budget inside the 3.5-second hard deadline. Cleanup starts at
the first queued synthetic mutation, retains a receipt only after a valid lease
and exact selection identity, and reconnects only after reinitializing XKB and
XInput state. A paste payload with no client receipt remains available for a
late real request for up to two seconds, then restores the exact original
generation and releases the pending operation. It blocks ordinary clipboard-
independent word correction for only 500 ms; physical Ctrl+Z then remains application input instead of
creating a synthetic Punto undo. The first expected-client text request begins
a 250 ms quiet-grace rather than immediately restoring the old clipboard.
Further text targets renew that grace within a one-second absolute cap, so a
multi-target paste keeps the replacement without allowing continuous probes to
retain ownership forever. A foreign owner during either phase is never
overwritten. Expected-client identity uses the X11 resource-id base; the
absolute receipt cap bounds the residual ambiguity if an X server reuses a
disconnected client's base during that window.

The final production source passed all 36 Release CTest targets in 454.04
seconds and all 37 Debug sanitizer targets in 471.31 seconds. The separately
isolated GTK clipboard sanitizer target passed 1/1. CLI passed 5315/5315,
packaging passed 919/919, and the exact CI clang-tidy selector passed all 25
production translation units. `git diff --check`, Python compilation,
ShellCheck, formatting of every changed C++ file, the Node bridge contract and
the GJS uint64 contract passed. The canonical full package passed lintian's
error gate; its remaining warnings are the documented manual-page and native-
package changelog naming debt.

| Iteration 19 candidate | SHA256 |
| --- | --- |
| `cpp/src/word_editor.cpp` | `4c9e932d110e79d05fea3822942e659fe677138a5ffbf9283a0e801d86452cd2` |
| `cpp/src/clipboard_manager.cpp` | `e603c917453eaf10fe2638a4324fd29a8d5f9d231a9778e8ea87c69118e8f3de` |
| `cpp/src/event_loop.cpp` | `dc6a2b73fdb5cb37e34f16378773befaa03e4d81cc13254061f5669521cb6d69` |
| `cpp/src/desktop_layout_client.cpp` | `9504a5f7b7cffa0984f071509c4acfff32e96e4d1aef96b411e2558981a11352` |
| `cpp/src/session_bridge.cpp` | `22782c0aba76cfdc39eee90d82930cff2621adc95dc38bdef29dea8fa63faec8` |
| Release daemon | `06f40edf81b288a7c977f23101b42a9a59ed07f6ae137c0b2e051e76c8199742` |
| Release session bridge | `5a4d644c8779f88ace760958f9373d4a5216da02019084cd10e1310e8e87ab48` |
| Release EventLoop driver | `5eb1d6ee55e75ac4f46a46ea7f159f5d5194703f8921f0305a235432c640cbfb` |
| Debug daemon | `b206a131d7908d5f41dc7642aa3a218dcfe2fdfa72e35fb6b5831b57cb2915d0` |
| Debug session bridge | `fae719eb70489b732e0f817efe3a5afc44bc7238d631a2ef2e2aee97df8c28d4` |
| Debug EventLoop driver | `935d4afd300b06b2405382c704f19f24ec317e771152ebf5a9108b6381665f7a` |
The canonical Debian checksum is published separately in `SHA256SUMS`, avoiding
a recursive package-hash claim inside a document embedded in that package.
These isolated gates bind the production binaries to the tested sources.
Enabling the extension, starting the user-session bridge, installing
the package and proving two real GNOME source transitions plus editor text are
separate host-session gates performed only after publication readiness.

## Iteration 20: fail-closed bridge recovery

Three faults found during the iteration-19 review were fixed without changing
the ownership model. After a confirmed desktop transition, failure to refresh
the XKB keyboard plan now stops before XInput2 observation. The collector also
rejects a closed XCB connection on entry. A retained failure lineage can be
used only once, after fresh text input, and is rejected when PRIMARY identifies
a new readable selection or its transport fails.

`DesktopLayoutClient` opens the user-runtime descriptor with `O_NONBLOCK`, then
retains the regular-file, owner, mode, link-count and size checks. A FIFO with
no writer is rejected before any extension call, and the same client succeeds
after the valid descriptor is restored. Its D-Bus pump processes at most 32
messages per slice and checks completion, cancellation and the deadline between
messages. Recovery preserves the original operation identity and never sends a
second `Switch`.

The unchanged source manifest passed 36/36 Release CTest targets in 481.97
seconds. Debug registers 37 unique targets. The main CI lane excluding the GTK
clipboard contract initially passed 35/36; one GTK shard reported a clipboard
callback timeout after successful product dispatch and without a sanitizer
diagnostic. That complete shard then passed 1/1, while its isolated VTE case
passed 10/10. The separate GTK clipboard ASan/UBSan lane passed 1/1 with only
the documented `detect_leaks=0` exception. The resulting unique target coverage
is 37/37; the ten reproductions are not counted as extra targets.

CLI contracts passed 5315/5315, clang-tidy passed all 25 selected production
commands, and the static, formatting, Python, Node, GJS and whitespace checks
passed. The local architect and corrective critic reported no unresolved
P0-P2. C2C PLAN revision 2 records Grok and GLM as
`INCONCLUSIVE_PROVIDER_UNAVAILABLE`, without treating either as approval, and
makes those two verdicts optional for this candidate. Independent C2C review of
the rebuilt package remains mandatory before commit, publication or installation.

| Iteration 20 source and tested binary | SHA256 |
| --- | --- |
| `cpp/src/word_editor.cpp` | `052a7e89de7e00a012f4f992454959731418f0e22008516a61bb6ab3b727c350` |
| `cpp/src/clipboard_manager.cpp` | `e603c917453eaf10fe2638a4324fd29a8d5f9d231a9778e8ea87c69118e8f3de` |
| `cpp/src/event_loop.cpp` | `a181f2cacd080efd24381b96bf74adce44407d33248e34b4e17860563bd938d6` |
| `cpp/src/desktop_layout_client.cpp` | `2892e398b40b1946aa61f2f3106ed97e750607d84d701b0483f75aa886d267ff` |
| `cpp/src/session_bridge.cpp` | `22782c0aba76cfdc39eee90d82930cff2621adc95dc38bdef29dea8fa63faec8` |
| Release daemon | `117bc6ed317c138de4647ab5d9f5a674cfaff6beefad7f88cb7f5ae8c37b222f` |
| Release session bridge | `5a4d644c8779f88ace760958f9373d4a5216da02019084cd10e1310e8e87ab48` |
| Release EventLoop driver | `7b3433b8dfdee8dec3ba35ccd00891ac3b2a2451b1defa92aa84e6afc076f8ee` |
| Debug daemon | `ed464d169742458f4e1ec1484523ed7b3e4deaa14623db1b9d15757527d928b4` |
| Debug session bridge | `3687b04f22d5a59dd768fd96b157c2ed84252155e980754228cbd0326151e3ac` |
| Debug EventLoop driver | `f73f32f5d0f286ba37a20992da41b72023675d37e0d8ca3c415c6be9a7c0fc72` |

The package checksum stays in `SHA256SUMS` and the C2C execution evidence. It is
not embedded here because changing this document changes the package itself.

## Iteration 21: production GJS D-Bus dispatch and provenance

The GNOME extension's D-Bus transport is now isolated in `dbusAdapter.js`.
`InputSourceBridgeCore` remains the only owner of helper authorization, leases,
operation identity, source validation and activation policy; the adapter only
normalizes the callback argument representation and encodes protocol replies.
This separation also allows the shipped handler implementation to run outside
GNOME Shell resources without replacing it with a fake.

A bounded `dbus-run-session` regression exports that same adapter with
`Gio.DBusExportedObject.wrapJSObject` and calls `BindHelper`, `GetState`,
`Switch` and `GetOperation` through real Gio dispatch. The pre-fix reproduction
showed that GJS supplies an already-unpacked value array, so the former
unconditional `parameters.deepUnpack()` rejected a valid call. The focused
post-fix CTest passes successful replies, version and digest rejections, exact
`uint64` fields, stored operation identity and one activation across replay.

Revision-3 provenance freezes both independently consumed protocol files,
the production adapter, its dispatch regression and the package/install
contracts. The revision-2 Debian checksum is intentionally stale after this
payload change. Complete Release, Debug sanitizer, static and package results
are recorded in the C2C/checklist evidence bound to the revision-3 manifest;
installation and real GTK/Chromium acceptance remain later, separately
authorized gates and must precede stable publication.

## Iteration 22: GNOME user-manager session attribution

The first controlled 2.8.14 activation exposed a native logind topology that
the original bridge rejected: GNOME Shell is owned by the correct session-bus
UID, but its user-manager process has no direct `sd_pid_get_session()` mapping.
The helper therefore exited with `TEMPFAIL` before `BindHelper` even though one
active, local X11 user session existed on `seat0`. Both controlled attempts were
rolled back to the exact official 2.8.13 package; no candidate daemon entered
the keyboard pipeline and GTK/Chromium live acceptance was not run.

Session attribution now authenticates the Shell unique D-Bus owner, PID and
Unix UID independently. A valid direct PID mapping remains preferred. Only
`-ENODATA` permits native per-UID logind inventory, and admission requires one
distinct active, nonremote, X11, class `user`, `seat0` session after filtering.
Unknown metadata errors, zero matches and ambiguity fail closed. The pinned
owner, PID and exact session decision are revalidated after binding, before
descriptor publication and before every forwarded request; an existing helper
never retargets to another session.

The production resolver is shared by the bridge, its injected native-query
contracts and a read-only host probe. The probe neither owns the bridge name nor
calls `BindHelper`/`Switch`; on the affected host it reports the authenticated
UID match and `user-inventory` selection of session 111. A production-shaped
private-bus test preserves the old exit-75 RED at
`pid-session-enodata-before-bind`, then runs the fixed helper through the shipped
GJS adapter/core and proves one activation across an identical replay.

Deployment preparation now uses a closed-stdin `dpkg` transaction with both
`--force-confdef` and `--force-confold`; downgrade also uses
`--force-downgrade`. An isolated user-namespace rehearsal performs a real
upgrade and downgrade of deliberately divergent conffiles, preserves the local
configuration and learned-data sentinel, reaches `ii` after both directions,
and rejects a missing rollback package boundedly. This is local preparation,
not authorization to replace the active host wrapper or install 2.8.14.

The final deployment wrapper validates both candidate and rollback artifacts
before the first package mutation. Activation is limited to a validated
1–120-second interval (60 seconds by default), receives `TERM` first and has a
two-second forced-stop grace. The package transaction similarly gives `dpkg`
30 seconds followed by a five-second forced-stop grace. The rehearsal proves
that an absent rollback artifact leaves the old package untouched and that a
genuinely hanging activation exits 124, invokes downgrade, restores a
configured old package, and preserves both the local conffile and learned-data
hash. This does not eliminate the residual risk of an interrupted real `dpkg`;
the controlled host activation remains a separate authorization gate.

The production-shaped old-code regression records
`OLD_RED stage=pid-session-enodata-before-bind exit=75`. The same private-bus
fixture then passes with the new test-seam helper and one activation across a
replayed `Switch`. The read-only production resolver probe reports an
authenticated Shell UID, route `user-inventory`, stage `inventory-selected`,
and session 111 without binding, switching, publishing an endpoint, or changing
the host. Startup coverage additionally replaces the Shell owner on a distinct
D-Bus connection between binding and publication. Runtime coverage separately
revokes authority for ambiguity, inactivity and a disappeared session, then
starts a fresh helper without retargeting the old identity.

After the final review fixes, Release passed all 40 registered targets in
481.60 seconds. The first complete Debug main run exposed one transient
one-shot `STATS` read timeout after the layout recovery had already produced
the expected text with the same daemon PID. That failure remains recorded; it
was not silently retried. The test's final asynchronous telemetry assertion now
uses the existing bounded exact-count oracle instead of one 500 ms socket read.
The focused Debug layout test passed 1/1 and then 10/10 consecutive runs. The
complete final Debug main lane passed 40/40 in 502.50 seconds with
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`; the isolated GTK clipboard
lane passed 1/1 in 0.28 seconds with the documented `detect_leaks=0` exception.
CLI contracts passed 5315/5315 and package contracts passed 921/921.

The exact CI clang-tidy selector covers all 27 production translation units.
ShellCheck, Python compilation, changed-file clang-format, Node syntax and
`git diff --check` pass. Separate Codex architecture and corrective reviewers
first rejected incomplete owner/session race coverage and an unbounded rollback
orchestrator; both gave PASS with no remaining P0–P3 after the causal tests,
artifact preflight, bounded activation and graceful package timeout were added.
These are local review gates, not substitutes for the independent C2C review.

Both Chromium shards, soak and failure-soak passed on the final Release and
Debug runs without a retry. The earlier correction-32 rejection remains part of
the historical evidence and is not reclassified as fixture-only. The new source
manifest, deployment-wrapper digests and rebuilt Debian provenance are kept in
the iteration-7 revision-5 C2C evidence. The Debian checksum is not embedded in
this packaged document because doing so would change the package recursively.
