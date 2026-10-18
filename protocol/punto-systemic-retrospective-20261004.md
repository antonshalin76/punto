# Punto: systemic retrospective and implementation plan

Date: 2026-10-04. Goal: one functioning, documented GNOME46/X11 RU/EN product,
not a succession of locally green release candidates. Branch
`codex/punto-hardening-2.8.6`, base `8f8e24a`. Existing user changes are retained.
C2C is historical only. No editor/browser module, agent-collab, GNOME Shell/IBus
restart, host/VM reboot or unaccepted host installation is authorized here.

## Coverage and confidence

The initial complete journal snapshot has514 events:368 incident records and146
resolution records,222 IDs without a resolution. The retrospective extraction
added one tooling incident; the indexed snapshot has515 events/369 incidents,
146 recorded resolutions/223 open IDs. These are reports, not369 independent
product bugs and not223 proven current defects.

The private, ignored index is
`.cache/codex-thread-workflows/punto-retrospective-20261004/case-index.json`.
Every incident has its exact journal line, ID and reported disposition.
Keyword routing is only discovery, never inferred root-cause proof. Validate
counts, unique IDs, referenced lines and the immutable source-prefix hash.
A recorded resolution is not automatically acceptance on today's artifacts.

The original task is recovered from thread
`01a06b8c-4493-7852-87ff-f6c61e0dca93`, rollout line10. Head and tail packets
preserve the original objective and latest permissions. Retrieval reports an
incomplete projection, ambiguous duplicates and malformed rollout line103004;
the retrospective does not claim to reconstruct unrecorded incidents. The
workspace catalog's57 August28 subagent threads do not represent the main
September/October thread, whose cwd is `/home/anton/Source`.

Labels below: **C** = cause supported by current source and retained causal
evidence; **O** = observer/tool/infrastructure failure, not proof of product
corruption; **H** = historical source/evidence, requiring final-artifact checks;
**U** = unresolved attribution. Native/private tests, ordinary installed
behavior, package behavior and host behavior remain separate evidence planes.

## Failure families and roots

| Family | Root and representative evidence | Owner and prevention | Current boundary |
| --- | --- | --- | --- |
| 1. Product capability lost during hardening | H: v2.8.6 README explicitly disabled automatic/manual mutation; `pc_87dba3430df0` records v2.8.8 reported complete while mutation stayed disabled. Safety/passthrough health was mistaken for fulfilled product behavior. | Delivery contract plus EventLoop/WordEditor: require actual correction before declaring working mode or releasing. | Editing exists now; partial installed PASS is not whole-product acceptance. |
| 2. Competing system-layout state | C: temporary XKB state diverged from GNOME source; `pc_39effce47dd7`, retained installed `selection_source_context_before`. A successful glyph replacement did not ensure subsequent input used the requested layout. | GNOME bridge owns source transitions; XKB independently observes effective state. X11Session publishes only current revision/generation. | Existing owner correction qualified locally; real full-cohort source parity remains pending. |
| 3. Desktop disruption | H: `pc_cbf095d31874` records an automated in-session Shell replacement destabilizing the workstation. RESTORATION iteration14 separately reproduces GNOME keyboard freeze without Punto. These are distinct incidents, not one guessed OOM cause. | Validation executor owns resource/session safety; narrow GNOME compatibility remains opt-in and never becomes a second source chooser. | Never restart Shell/IBus. Serialize bounded private GUI/build and guest-input lanes; no host keyboard tests. |
| 4. Cold accessibility dependency | C: `pc_395c8efd2b5d` shows ordinary Chromium provider absent when standard accessibility is off, hidden by pre-enabled fixtures; `pc_cd3e2f806227` covers enrollment persistence. | SessionBridge owns first activation, persisted enrollment and opt-out; keep ScreenReader=false. | Same-source enabled guest20/20 passed; same-PID re-enable provider0 remains explicit, not falsely recovered. |
| 5. Session and credential attribution | C/O: active-user topology and user namespaces matter; ordinary/root Python diagnostics are correctly rejected by the private daemon endpoint (`pc_7e4e034f5ff5`, `pc_76eb91221b49`). | SessionResolver/session helper own authenticated user/bus/process lineage. Diagnostic projection must strip private lease authority. | Public helper read passes source11/11 and actual private-bus3/3 suites; ordinary installed acceptance pending. No private-auth weakening. |
| 6. Child/range provenance and ABA | C/U: first-key child provenance, idle preparation and unbound prebinding generations could become stale; `pc_e946ab5539d4`, `pc_b00abf3058b1`, `pc_8bd38c4d4a23`. A parent window or matching text is not exact child authority. | AccessibleContext owns fresh provider/token/range; WordEditor admits the exact edit. Keep bound-token/range and focus/session checks. | Causal prebinding RED/GREEN exists. Earlier intermittent origin failures are not universally attributed or closed. |
| 7. Physical input ordering/liveness | C: buffered prefixes, partial SYN frames, held modifiers and backpressure can reorder or strand input (`pc_427da18dba6d`, `pc_1e797e095ddf`). | EventLoop owns every physical press/release/frame and bounded queue terminality. WordEditor cannot mint physical input provenance. | Preserve consumed Pause, exact delimiter release and next-symbol proof; fail explicitly on EOF/output failure, no replay rescue. |
| 8. Async callback and provider lifetime | C: synchronous broker work blocked independent requests (`pc_fe083d60d6f0`); provider loss differs from registry/bus loss. The single global observation slot also lets optional idle preparation reject a real observation (`pc_2c58cbacf166`, final339 warm/read overlap). Cancellation is not destruction or cancellation of accepted editor writes. | SessionBridge/AccessibleContext own async calls, generation fencing, drain and callback lifetime. Each pending context retires exactly once. Only preparatory observations may yield to actual observations; actual requests must not evict each other. | Preparation admission correction and causal/late-peer controls pending. Existing private/public controls alone do not prove this interleaving or installed behavior. |
| 9. Clipboard dispatch versus restoration | C: queued/possibly sent paste differs from unsent refusal; early restoration or orphan pending state loses required payload. `pc_3f1774d6ced0` spans actual publication/request service and unresolved browser disposition. | WordEditor owns dispatch certainty; ClipboardManager owns publication/transfer lifetime and conditional restore. Preserve foreign copy and all representations. | Causal raw-X controls qualified; do not infer every old browser failure from one CM fix. |
| 10. Clipboard concurrency/server cut | C: ordinary readers were mistaken for incomplete identity history, then conservatively rejected by conditional publication after the guard was released. Preparing absent CLIPBOARD could also reject valid owned-PRIMARY reads. | ClipboardManager owns the existing checked server cut and one shared event budget. Serve OLD-generation requests outside the grab, retire deferred reads, then revalidate the original identity before one publication write. No blind restoration, ownership adoption or renewed deadline. | Both publication causes have real-X RED/GREEN and independent source review. Current full source/native and installed acceptance remain open. |
| 11. History, inverse and shared finalization | C/H: stale/reset child history and mixed-layout punctuation could oscillate or block fresh recovery; manual queuing cleared active origin/word/manual flags together. Tab initially bypassed common completion. C: redundant SET_STATUS cleared fresh provenance in primary and secondary while retaining the word (`pc_c8f1bd8f3b9c`). | EventLoop owns active-word trio and correction finalization/undo/learning/sound; WordEditor owns retained edit receipt. A status generation acknowledgement is not an effective mode change. Invalidate only the authority actually lost. | Causal primary/isolated-secondary REDs and13 actual GREEN controls qualify effective-value reset guards; full339 native and installed lifecycle acceptance pending. Historical cold-Tab attribution remains unproven. |
| 12. Deadline and completion semantics | C/U: source proves expired non-cancelled Wait can be labelled generic context failure instead of layout timeout (`pc_754d5c4f211a`). Historical occurrence remains unattributed. O: undo expiry was measured from trigger rather than completed dispatch (`pc_c35c30f836cf`). | WordEditor owns typed outcomes and original absolute deadlines; collector observes owner-thread completion, not latency guesses. UNKNOWN/in_progress never means safe cancellation. | Deterministic layout RED1/1, GREEN1/1 and7 existing recovery controls pass. Final integrated327/327 collector dry controls pass; installed acceptance pending. |
| 13. Observer coherence/ABI/readiness | O: owner query and event generation came from different cuts; GTK cached TARGETS preceded known publication (`pc_b5c28392cdee`). TIMESTAMP native INTEGER32 used LP64 length8; nonexistent GI method, XKB pointer ABI and import-order mistakes created false failures/greens. | Existing independent XFixes/GTK reader owns fresh catalogue and coherent metadata; exact native API contracts and import pins. | Native causal observer controls and325 dry controls pass. Never label these alone as product clipboard loss. |
| 14. Incomplete UI completion/cleanup | O/C: Tab text/layout was observed before focus/delimiter/clipboard completion; cleanup then masked the primary failure (`pc_71d51a9ff65d`). A live Sound dialog may later overwrite restored config. | Collector awaits exact completed postconditions; cleanup retires writers first, restores exact objects/status, preserves original cause with safety precedence. | GTK Tab cases passed within42 operations; complete lifecycle cleanup remains pending. |
| 15. Configuration/learning/UI updates | C/H: stale full SettingsData could overwrite unrelated concurrent config on Sound Save (`pc_1e2fcbcc17a1`); request generation was mistaken for reload commit; durability/reset requires an acknowledged generation. | Config persistence owns section-specific merge; actual tray action owns its dialog; daemon owns learning epoch/store/reset. | Current source/contracts corrected locally; real Save, attributed sound ON/OFF, exclusions/reset/restart not yet accepted. |
| 16. Build, CI and evidence qualification | O: zero-test CTest, wrong class/imports, prewarmed dependencies, unread stderr pipe, aggregate scheduling bounds, wrong tools/ABI and stale compile database hid or invented failures. `pc_14e7d4131798`, `pc_af621836f9c5`, `pc_c8f68cd8502d`, `pc_3a7cf5b12d74`. | Existing runners own declared nonzero selector sets, dependency closure, bounded diagnostics and source/artifact fingerprints. Separate operation deadlines from fixture scheduling. | Latest whole native339 is336 PASS/three FAIL, zero errors/skips/unfinished. A later focused PASS cannot replace it. |
| 17. Installation/update/rollback | H/O: old prerm stopped the pipeline; conffile prompts, incomplete backups and public-socket promotion races caused ineffective installs or repeated rollback (`pc_da22ebdb8c85`, `pc_178643461519`, `pc_93836352ed63`). | Existing dpkg transaction/orchestrator owns exact targets, writer death, config preservation, explicit activation and one rollback executor. | Archived exact285 upgrade succeeded only for its old package; current overlays are not a final coherent DEB. Fresh/rollback/soak pending. |
| 18. Workflow and publication | O: C2C/OAuth/tunnel/reviewer/tool failures, narrow verifier amendments and exhausted budgets displaced real product proof (`pc_236068795dd2`, `pc_cb22375f319a`, `pc_fbbeed902a2e`). C: `pc_85d3f98b7a39` current GitHub principal is not the approved release actor. | Local evidence graph and one integrator; provider availability is not a code verdict. Final publication requires exact source/package and authorized principal. | C2C disabled; no new INIT/review relay. `anton-shalin` cannot substitute for authorized `antonshalin76`. |

Historical resolution records remain preserved. This taxonomy groups related
incidents without collapsing distinct causes or claiming every old symptom has
the same cause. Exposed desktop credentials (`pc_0728b8e65c9c`) are a separate
security item: no environment dumps or secrets in evidence; rotation cannot be
claimed without the owning account's verification.

## System contract: one owner per state

| State | Authority | Consumers must prove |
| --- | --- | --- |
| Session/user/provider lineage | SessionResolver + SessionBridge/AccessibleContext | Current UID/PID/start/bus generations; fresh child, not merely ready infrastructure |
| System source/keyboard state | GNOME bridge; X11Session revision-fenced observation | Same source generation, requested role and effective/locked XKB group; next ordinary glyph |
| Physical sequence/queue/held keys | EventLoop | Entire lifecycle/frame ordering, exactly-once delimiter release, no stuck modifiers |
| Runtime mode and shared intent generations | Primary control-plane publication; EventLoop consumes each applied generation | Acknowledge redundant status intent without clearing fresh word provenance; actual value change, forced initialization, config change and active-macro cancellation remain fences |
| Text/range/dispatch certainty | WordEditor | Exact admitted child/range; mutation finished or cannot happen later before declaring refusal complete |
| Clipboard publication/restore | ClipboardManager | One coherent cut, exact representations, current owner and no overwrite of foreign data |
| Correction/undo/learning | EventLoop + UndoDetector/store | Completed correction anchor, exact inverse, current epoch and durable acknowledgement |
| Config/dialog/audio | Config persistence, tray dialog, SoundManager | Section-specific Save, committed generation, attributed player/WAV/PCM; quiet OFF negative |
| Deployment evidence | Existing transaction/orchestrator + qualification collector | One package fingerprint, exact rollback, real installed behavior and actual restore |

Adapters project or transport these states; they do not duplicate decisions.
Do not add a generic FSM, scheduler, retry framework or second validator runtime.
Reuse GTK/GIO/XCB/sd-login/Pulse and existing repository lanes. New code is justified
only by an actual capability or stronger invariant, not by another passing counter.

## Finite implementation plan

**P0 — Reconcile and freeze.** Preserve all cases/failures; validate inventory;
review this owner map independently. Tie every remaining change to a family,
changed observable contract and existing deterministic seam. Current baseline:
native335/336, installed42 operations,327 dry controls. None is release acceptance.

**P1 — Close the interaction boundaries together.** Implement the expiry observer
completion fence (family12/13), prove/fix expired layout classification through
the existing hook (family2/12), and qualify the stripped public source observation
(family5/8). No change to2.5s undo, macro/cleanup budgets, private authentication,
source owner or Pause absorption. Preserve cancellation, late completion,
foreign ownership, busy capacity, shutdown and fresh-input recovery controls.
Close the complete336 native/legacy selector inventory on the final frozen
candidate, execute the public observer's real GDBus/GJS controls, and pass the
matching source gates before P2. Reuse only evidence with unchanged inputs;
installed42 and325 dry controls cannot substitute for this native gate. Run one
final aggregate after integration, not a full rerun after every cached proposal.

**P2 — Whole installed product.** Run one frozen ordinary guest cohort covering
both directions of word/selection/case/translit, auto Space/Tab, literal Tab,
empty refusal followed by recovery, next glyph/source parity, immediate/expired
undo, CLIPBOARD representations, modifiers, owned canonical terminal, durable
learning/exclusions/reset/restart, actual Sound dialog Save and attributed audio
ON/OFF. Follow with finite sustained-operation proof; preserve unsupported
same-PID provider disposition explicitly. A first failure yields one owning-layer
counterexample, not a new verifier project or acceptance by unchanged retry.

**P3 — One deliverable.** After source/behavior freeze build one2.8.14 DEB in a
disposable source snapshot. Prove fresh install, exact archived2.8.5 upgrade and
observed rollback; preserve user/system config, exclusions, enrollment and input.
Run the existing1800s/200-operation soak on accepted installed bytes. Bind each
test to package/payload/process/dependency identities. Rebuild only on invalidation.

**P4 — Host and release.** Only after P3: reversible host update, critical actual
user-session checks without Shell/IBus restart, final review/commit/push/tag and
publication as `antonshalin76`, remote readback of exact release artifacts.
Missing account authorization blocks publication, not local stabilization.

Dependency graph: `P0 -> P1 -> P2 -> P3 -> P4`; P1's disjoint cached work can run
in parallel, but the integrator alone edits shared source and owns serial builds,
private GUI and guest input. Graph PASS records are evidence indices, not truth
or permission. Keep the historical C2C task/iteration only as provenance.

## Current observation-admission implementation, 2026-10-04 16:49 UTC

The completed339 inventory retained336PASS/3FAIL. Actual helper traces and a
controlled old-code counterexample establish preparation blocking a working
read at the one global AccessibleContext slot. Preparation now yields to an
actual read while actual reads remain single-flight and cannot evict each other.
Preparation has no cached-origin authority. Existing owners pass explicit
private preparation intent; GLib cancellation and completed-job guards retain
late reply/retired-peer safety. Product deadlines, authentication and selection
preflight are unchanged. Independent BDD/mapping/source/SRP review passes.

Strict current-byte causal controls pass2/2; affected GTK controls pass10/10,
Chromium controls5/5. Ordinary Release34/34 passes76.39s, ASan/UBSan/LSan
Debug35/35 passes85.00s and strict31 static passes with all87 fresh pins matching.
The complete341 inventory passes341/341 with zero failures/errors/skips/
unstarted/unfinished/retries/waivers and all165 source/Release pins matching;
independent terminal/source/SRP review accepts P1. All339 original selectors remain, plus
two admission controls. One deliberately suppressed XKB observation can reach
either existing preflight-timeout branch; its oracle accepts only those two,
preserving literal Tab, untouched text/clipboard and fresh recovery criteria.

The correction changes eight existing C++ files (+31/-15, net16), two existing
XML contracts (+44) and their signature test (+2). No new runtime file, queue,
pool, cache authority or fallback is introduced. Current Release daemon is
`a98e4f7a`, helper `a03233d3`; installed guest bytes remain unchanged.

The P0-P4 checklist remains the route. P0 and P1 technical gates are supported;
installed78, one-package lifecycle/soak, host and release remain unaccepted.
The earlier cold-Tab attribution remains unproven rather than inferred from
a subsequent PASS. Original failed339 and initial test cleanup logs are retained.

## Historical effective-status implementation, 2026-10-04 15:47 UTC

Primary and secondary redundant-status provenance loss are causally reproduced
and corrected at EventLoop. Actual Chromium RED1 (5.624s) and isolated GTK
secondary RED1 (8.398s) are independently accepted. A first secondary failure
included a legitimate config reload and remains unqualified; the final test
requires both peers to acknowledge the same config before typing and keeps it
unchanged across SET. Publication, status generations, durability, forced setup,
actual mode/config changes and active-macro cancellation remain unchanged.

Chromium5/5 and GTK8/8 focused controls pass. Current ordinary Release34/34
passes77.99s; ASan/UBSan Debug35/35 passes84.41s with leak detection enabled.
Current Release daemon is `84b69e71`, helper `61622825`; EventLoop source
`41756f7a`. Strict31-unit static and full339 GUI gates are still open. The final
inventory preserves all336 original cases and adds three causal profile cases.
Independent source/SRP and pre-execution review pass. Production correction
adds5/removes2 lines (net3); no new file, state authority or generic framework.

P0 remains the only accepted complete stage until P1 finishes. Installed78,
package lifecycle/soak, host and publication are not accepted by these source
results. Prepared guest pins still require rebinding to the final current bytes.
The historical cold-Tab cause remains unproven, not attributed by a later PASS.

## Historical pre-effective-status implementation checkpoint, 2026-10-04

P0 is independently accepted: the immutable515-line journal prefix, all369
unique incident references and reported resolution links are checked. P1 has
three integrated boundaries, not a claim that all historical bugs are closed:

- Undo expiry checks now anchor to authenticated owner-thread completion; the
  production2.5s window is unchanged. Final integrated dry collector327/327
  passes; ordinary installed expiry acceptance is still pending.
- Expired non-cancelled layout Wait is classified by the original absolute
  deadline. Deterministic RED1/1, GREEN1/1 and7 existing recovery controls pass.
  This does not attribute the previous intermittent whole-run failure.
- Public helper DesktopObserver1 exposes bounded same-user lease-free state;
  private daemon authentication/capacity are unchanged. All entrypoints are
  registered before readiness publication. Source11/11 and actual private-bus
  3/3 suites pass; ordinary installed GTK/Chromium source parity is pending.

The final native aggregate executes all retained336 selectors/13 serial
packets:335 PASS/one FAIL, zero errors/skips/unfinished. GTK/VTE221/221 passes;
Chromium114/115 passes. The previous layout-cleanup selector now passes, but
native `test_extensionless_automatic_tab_orders_next_field` refuses at
`editor_origin_missing` before clipboard/selection mutation. Receipt:
`.cache/codex-thread-workflows/punto-autonomous-retrospective-native-final-20261004/run-20261004T135032Z-006f8e4a/terminal-receipt.json`,
SHA256 `1910da8231c6dc60d6189ec799d21efc2e30ce278e3cf4ea78fac512eb17b123`.
The165-input freeze protects authoritative runtime XML; missing-dependency
RED and exact drift-rejection GREEN are verified. This aggregate is NOT_PASS,
not near-complete product acceptance.

Independent source review finds no unconditional origin loss through normal
letter/Tab finalization. Early session/config refusal or a later clear remains
possible, not proved. Test-only lifecycle attribution now distinguishes entry,
actual readiness/config flags, result and clearing; no provider token or text
is logged. Exact sandbox forwarding is tested. One cold case and the original
ordered seven-case predecessor packet pass with valid trace (3.290s/14.870s).
These do not reproduce the failed interleaving and do not close
`pc_a480727b4efc`. The next P1 action is a bounded causal counterexample that
distinguishes first-key admission from later clearing, followed by one
owning-layer fix and the existing recovery controls. No unchanged retry-to-green,
prewarm authority, stale-token reuse or deadline expansion is allowed.

After attribution integration, ordinary Release34/34 and ASan/UBSan Debug35/35
contract gates pass (83.33s/93.17s); leak detection remains enabled. The complete
31-unit strict static gate passes with exit0; all87 frozen inputs match. Its first
attempt used an inappropriate180s build budget and stopped at23/31; that log
is unqualified. The successor retains all inputs/checks and resource caps with
a separate finite900s analysis budget; this does not change product deadlines.
Terminal log is `platform/evidence/autonomous-20261002/retrospective-all31-static-final-20261004.log`.

Current Release daemon SHA256 is
`cabd1575a1ef47472eb20a14b6d24bbfd9d17737c041a794af01802a51103086`;
helper remains `61622825`. The original336 proof used daemon `7153bcb9` and its
source freeze predates the test-attribution integration. Do not combine them
into a current whole-suite PASS. Read-only guest inspection at13:52 still finds
old daemon773359/helper576673, helper58c4ec85 and protocol705e8e01; both configs
retain598e9282, Shell1356 and IBus1460. No guest activation/input was performed
in this retrospective work.

The prepared successor guest overlay handles daemon/helper/authoritative XML
as one backed-up transaction before any owned helper/pipeline activation.
Its exact old-overlay RED has four assertion failures; the corrected final
mechanical controls pass9/9, including missing-command and rollback negatives.
An earlier apparent GREEN with `check_hash: command not found` is explicitly
invalid. Mechanical preparation is not installed rollback proof, and its
`7153bcb9` daemon pin is now historical: repin a coherent successor only after
P1 acceptance. No Shell/IBus restart, host installation or release occurred.

P0 is accepted; P1 is incomplete; P2-P4 are not accepted. One of five sequential
delivery gates is closed, not an estimate that only20% of engineering is done.
No numerical completion/date claim can follow from335/336 partial behavior.

The reviewed public-helper packet changes seven existing maintained files:
832 lines added/14 removed, including runtime341/6, mirrored XML28/0 and tests
463/8; zero maintained files added/removed. This growth buys the new bounded
observation capability and controls, not a refactor-only code reduction. The
collector adapter changes two existing files: source78/74 and tests126/56;
zero new runtime module/framework. The typed layout correction adds5/removes1
runtime line. Cache-only deployment preparation is separate: overlay46/22,
tests82/2, not executed or installed. These are scoped packet deltas, not a
sum for the much larger pre-existing dirty tree or a claim that custom runtime
decreased. Shared ownership and existing GIO/XCB/sd-login mechanisms remain;
no new general FSM/parser/scheduler/retry engine was introduced.

## Subsequent installed validation: shared read history

The preparation/range/lineage corrections subsequently passed the exact native
344 inventory (receipt2311c4e9,2026-10-04 21:20:39UTC), Release34, Debug35 with
LSan and static31. Those results supersede neither the earlier failed snapshots
nor the installed product gate. The coherent6354dd9a/e9b9f3a9 guest then passed
34 installed operations and refused the first GTK TextView literalTab. Actual
entry diagnostics identify PRIMARY read_origin at selection_confirmation with
660ms remaining, before paste publication; cleanup obscures that entry stage.
Text and clipboard remain intact, enabled status is independently restored,
and the observer timeout/cleanup failure remains part of the failed receipt.

Two real-X causal controls establish a shared ClipboardManager defect: normal
CLIPBOARD requests interrupt stable PRIMARY origin capture and, independently,
post-notify header retirement. The previous two-request contract explicitly
expected that false refusal. Its replacement requires exact current text/full
identity, attributed peer replies and unchanged clipboard generation/restoration.
The actual160-event incomplete-history negative, expiry, ABA, connection and
guarded-publication controls remain. Both causal tests go RED before their
respective production correction and GREEN afterward.

Only those two read fences now finish bounded queued history. The common64-event
limit, original deadline and full owner/timestamp/generation/connection checks
remain; publication under a server grab still yields and releases the grab before
serving a peer. No new runtime owner, retry, thread or deadline is introduced.
This source proof does not establish every earlier cold-Tab cause. Current full
source/native and installed qualification must finish before package acceptance.

## Validation and stop rules

- Before each heavy gate: reuse matching evidence or name the exact invalidation.
  Keep command, inputs, artifacts, environment, result and log in one ledger.
- Whole behavior means completed text AND range/focus/source/next input/clipboard/
  modifiers. Process health, dispatch count, intermediate text and cache proposals
  cannot independently establish that conjunction.
- Start from ordinary desktop dependency state; no fixture-owned pre-enablement
  can substitute for product enrollment. User opt-out is not silently reversed.
- For changes: reviewed BDD/owner map, causal RED, unchanged-oracle GREEN and
  final source/SRP review. Reuse one complete packet across applicable skills.
- Resource ownership: build2G/2CPU/Tasks128; private GUI2G/2CPU/Tasks256; original
  per-operation bounds, finite enclosing commands and continuous status updates.
  Cap actual compiler fanout; CPUQuota alone is not a job-count limit.
- A genuine safety failure stops input, retires owned writers, preserves failed
  receipt and restores exact original state when demonstrably safe. No blind
  replay, force enablement, restart rescue or upgrade of UNKNOWN to success.

This is an author-derived integration plan informed by history, not a claimed
statistically learned workflow or a guarantee that no future bug can exist.
