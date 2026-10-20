# Native editor coverage, 2026-10-20

A toolkit fixture does not qualify every application using that toolkit. These
results distinguish native provider discovery, complete scratch-document
behavior, sanitizer validation and installed-host acceptance.

## Shared contract

The product owns editor admission, exact selection/origin proof, layout changes,
text mutation and refusal/recovery. Tests only start isolated applications and
observe their own documents. No application-name allowlist, editor extension,
runtime dependency or alternative dispatcher was added.

A complete slice checks Pause, the next RU glyph, Shift+Pause, Ctrl+Pause,
selected case, transliteration, AUTO after empty Pause, preserved CLIPBOARD,
released modifiers and unchanged daemon/helper PIDs. Representable transliteration
uses the existing direct replay, temporarily selects the required group and
restores the original group. Cross-group case-only edits retain their independent
paste path. Unrepresentable text and literal Tab still use strict paste receipts.

## Validated results

| Application / mechanism | Observed behavior | Remaining gate or limit |
| --- | --- | --- |
| GTK3 Entry/TextView | Existing full native suite, new context/Unknown and refusal/recovery cases. Installed-host five hotkeys/AUTO/context/next glyph/clipboard PASS. | The configured rollback window and learned exclusions are preserved. |
| VTE / canonical PTY | Existing counted-word, Tab, undo and fail-closed selection tests. | New Unknown/context heuristics require a proven GUI editor, not terminal fallback. |
| Chrome/Chromium plain/rich controls | All final Chromium shards and existing soak contracts pass. Actual installed Chrome complete plain/rich host slice PASS. | Inconsistent non-BMP ranges and nonportable clipboard targets remain safe refusals. |
| Qt5 QLineEdit/QTextEdit/QPlainTextEdit | Native9/9 PASS, including two same-PID Qt/GTK providers, all hotkeys and AUTO. | Password/ambiguity/foreign-root/cycle/129-node negatives remain enforced. |
| Telegram Qt6.11.2 | Actual installed app: five hotkeys, both directions, Unknown AUTO, one-letter context, next RU glyph PASS. Two same-PID Qt/GTK providers are checked individually. | User-authorized Saved Messages field verified empty before/after; no Enter/message; clipboard, initial layout and released keys PASS. |
| GTK4 Entry/TextView | Release2/2 and final ASan/UBSan2/2 complete slices PASS. Backward Entry PRIMARY is genuinely empty; exact native reorientation restores real PRIMARY. | Reorientation starts only after a positive original-token/range proof; new-range confirmation remains300ms and the absolute macro deadline is unchanged. |
| GNOME Text Editor | Actual installed app with a fresh owned document: all five hotkeys, AUTO and next glyph PASS. | Private Xvfb fixture uses GSK_RENDERER=cairo; host app settings unchanged. |
| VS Code / Electron | Actual installed app, private profile, saved-document oracle: complete slice PASS. | Profile enables editor.accessibilitySupport; default-profile and unrepresentable paste are not qualified. |
| Firefox / Gecko | Actual installed snap, private local textarea: complete slice PASS. | Bare Alt opens browser UI; existing RCtrl+Alt+Pause selected-case chord is used. |
| SWT Text single/multi | Installed DBeaver SWT GTK JAR, owned Java scratch editor:2/2 complete slices PASS. | Actual DBeaver launcher sets NO_AT_BRIDGE=1; SQL StyledText/application not qualified or reconfigured. |
| WebKitGTK2.4.1 | Actual local HTML tree exports editable content from a separate WebProcess PID. | Explicit SKIP77: exact-window-PID admission does not authorize that cross-process provider. Not a functional PASS. |
| LibreOffice Writer26.2.6.3 | Actual soffice.bin window PID and GTK3 backend verified in a fresh private profile. No corresponding AT-SPI root within5s, including SAL_ACCESSIBILITY_ENABLED=1. | Native provider prerequisite unresolved; no user profile/settings changes and no functional acceptance claim. |

Successful application slices are recorded in task-cache logs:
final-product-app-acceptance.log (Code/GNOME Text Editor/Firefox3/3),
final-product-surface-asan-matrix.log (GTK4 ASan/UBSan2/2),
final-product-surface-asan-negative.log (six deadline/receipt negative controls),
final-product-surface-asan-chromium.log (six rich-text/receipt/Tab cases),
final-owner-qt.log (Qt), swt-native-titlefix.log (SWT).
These are source-stage results, not proof of installation or release.

Installed2.8.16 host evidence is separate: deploy-feature-2816/host-gtk-eligible.log,
host-chrome.log, host-feature-burst.log, host-telegram-cleanup.log and
host-telegram-auto.log. All exit0. Source closure combines the original54-gate
run, affected6/8 recheck and final2/2:53 PASS, one explicit WebKit SKIP. The
failed original runs remain recorded. Package lifecycle107/107 PASS; no GitHub
publication is implied. Explicit desktop layout shortcuts reset contextual
history; contextual acceptance uses uninterrupted physical typing.

## Causal corrections retained

- Unknown words were explicitly excluded by AnalysisWorkerPool; strong absolute
  score plus multiple bigram/trigram matches now admit only well-supported cases.
- Contextual right-first completion could restore the old source after correcting
  the right neighbor; each successful history record now retains its own target.
- Multiple providers were mistaken for ambiguous focus even when only one
  provider exported a focused editable. Every provider is now checked.
- GTK4 lacks Collection. Native GetState/GetChildren discovery is bounded to128
  distinct objects, rejects cycles/foreign references and retains the deadline.
  A redundant depth16 guard rejected a valid65-node GNOME editor tree; it was removed.
- Qt Text properties require typed Properties.Get, not GetAll.
- GTK4 reverse selection exposes an empty PRIMARY despite a valid native range.
  Only exact bounded reorientation is allowed; native text does not replace PRIMARY.
- Code's accessibility textarea is not the whole saved model. The oracle saves
  only its own scratch file through Ctrl+S. Initial native selection readiness is
  pinned to the original child, and pre-dispatch same-payload republication needs
  a fresh exact native range. Armed paste identity is never refreshed.
- An unchanged strict PRIMARY receipt needs native child continuity, not a second
  interpretation of browser hypertext offsets. Refreshing a receipt or using the
  faster confirmation still requires an exact native range; selected-scope
  admission retains its full range proof.
- Slow native discovery exhausted the short GUI preparation phase. Preparation
  uses the existing global deadline with cleanup reserved; individual new-range
  confirmation and pre-publication expiry still retain their300ms refusal bound.
- Representable transliteration needlessly used clipboard across groups. Direct
  replay already supports verified group hops and restores the original group.
- Held-Tab paste verified its layout only after250ms client settle and could miss
  the reserved verification boundary. Layout verification now precedes settle;
  fresh child/context checks still precede successful completion.

## Reproduction and safety

Use punto-event-loop-layout-bridge-e2e-driver with
PUNTO_REAL_LAYOUT_BRIDGE_E2E=1 for external native applications. The legacy
mocked-layout driver now refuses that incompatible pairing before initialization.

tests/test_editor_matrix_e2e.py reuses the private bwrap/Xvfb/DBus/PID/network
fixture. Default cases are GTK4 Entry/TextView; Code, Firefox, GNOME and Writer
are explicit selections through PUNTO_EVENT_LOOP_E2E_TEST. Qt, SWT and WebKit have
their own compact runners reusing the same fixture.

Native processes use fresh profiles and bounded scratch documents, no network
or user documents. Code's --no-sandbox, Firefox's nested-sandbox accommodation
and WebKit's nested-test flag apply only inside the mandatory outer sandbox.
No GNOME Shell, IBus, GDM, host browser profile or user session is restarted.

Never poll ObserveEditor during a mutation: the helper has one foreground job.
Read GTK4 Text through CharacterCount and GetText(0,count), not GetText(0,-1).
Record binary hashes and terminal producers before consumers. Keep installed
host acceptance and explicit unsupported capabilities separate from toolkit PASS.
