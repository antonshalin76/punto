#!/usr/bin/env python3
"""Native Chromium retained-PRIMARY regressions in the private GTK sandbox."""

import json
import os
import pathlib
import re
import resource
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.parse

import test_event_loop_gtk_e2e as gtk


def browser_path():
    return next((path for name in ("google-chrome", "chromium", "chromium-browser")
                 if (path := shutil.which(name))), None)


def process_status(path):
    try:
        return path.read_text().rpartition(")")[2].split()
    except (FileNotFoundError, ProcessLookupError):
        return None


def browser_group_has_live_members(group):
    for path in pathlib.Path("/proc").glob("[0-9]*/stat"):
        status = process_status(path)
        if status is not None and int(status[2]) == group and status[0] not in ("Z", "X"):
            return True
    return False


def process_start_time(pid):
    status = process_status(pathlib.Path(f"/proc/{pid}/stat"))
    return None if status is None else status[19]


class ChromiumE2E(gtk.EventLoopGtkE2E):
    def setUp(self):
        super().setUp()
        self.prepare_word_editor()
        self.assertEqual(gtk.ipc_request(b"SET_STATUS 0\n"), b"OK DISABLED\n")
        self.profile = pathlib.Path(tempfile.mkdtemp(prefix="punto-chromium-"))
        self.browser = None
        self.addCleanup(self.cleanup_browser)
        page = self.profile / "page.html"
        second_value = ("" if self._testMethodName in {
            "test_extensionless_first_word_in_another_field",
            "test_extensionless_physical_tab_preserves_next_field_correction",
            "test_extensionless_automatic_tab_orders_next_field",
            "test_extensionless_redundant_status_preserves_automatic_tab",
        } else "ghbdtn")
        layout_sources = {
            "test_extensionless_current_ru_word_with_en_desktop": "привет",
            "test_extensionless_current_en_word_with_ru_desktop": "ghbdtn",
            "test_extensionless_selected_ru_word_with_en_desktop": "привет",
            "test_extensionless_selected_en_word_with_ru_desktop": "ghbdtn",
            "test_extensionless_selected_ru_emoji_with_en_desktop": "привет🙂",
            "test_extensionless_emoji_selection_refuses_and_recovers": "ghbdtn🙂",
        }
        delimiter_mode = {
            "test_extensionless_delimiter_unchanged_range_refuses_and_recovers": "unchanged",
            "test_extensionless_delimiter_foreign_range_remains_selected": "foreign",
            "test_extensionless_delimiter_late_selection_refuses_and_recovers": "timeout",
        }.get(self._testMethodName)
        rollback_mode = self._testMethodName in {
            "real_layout_bridge_paste_retains_payload_during_layout_settle",
            "test_extensionless_unsent_receipt_refusal_restores_and_recovers",
            "test_extensionless_unsent_context_refusal_restores_and_recovers",
            "test_extensionless_unsent_chord_expiry_restores_and_recovers",
            "test_extensionless_unsent_rollback_preserves_foreign_clipboard",
        }
        if self._testMethodName in layout_sources:
            first_control = ('<input id="a" autofocus value="' +
                             layout_sources[self._testMethodName] + '">')
        elif delimiter_mode:
            first_control = '<input id="a" autofocus value="hELLo.">'
        elif self._testMethodName == "test_extensionless_current_word_uses_actual_literal_tab":
            first_control = '<textarea id="a" autofocus>ghbdtn&#9;</textarea>'
        elif rollback_mode or self._testMethodName in {
            "test_extensionless_unrepresentable_selection_restores_plain_clipboard",
            "test_extensionless_unrepresentable_selection_preserves_rich_clipboard",
            "real_layout_bridge_paste_retains_payload_during_layout_settle",
        }:
            first_control = '<input id="a" autofocus value="ghbdtn☃☃">'
        else:
            first_control = '<input id="a" autofocus>'
        delimiter_script = ""
        delimiter_state = ""
        rollback_script = ""
        rollback_state = ""
        cleanup_script = ""
        cleanup_state = ""
        if self._testMethodName in {
            "test_selection_revalidation_failure_collapses_and_recovers",
            "test_post_cleanup_right_same_text_selection_rejects_receipt",
            "test_same_text_control_fails_when_identity_check_is_bypassed",
        }:
            cleanup_script = (
                'let rightDown=0,rightUp=0;'
                'a.addEventListener("keydown",e=>{if(e.key==="ArrowRight")rightDown++});'
                'a.addEventListener("keyup",e=>{if(e.key==="ArrowRight")rightUp++});')
            cleanup_state = "[rightDown,rightUp],"
        if rollback_mode:
            rollback_script = (
                'let pasteEvents=0,pasteChords=0;'
                'a.addEventListener("paste",()=>pasteEvents++);'
                'a.addEventListener("keydown",e=>{'
                'if(e.ctrlKey&&e.code==="KeyV")pasteChords++});')
            rollback_state = "[pasteEvents,pasteChords],"
        if delimiter_mode:
            action = {
                "unchanged": "e.preventDefault();",
                "foreign": "e.preventDefault();a.setSelectionRange(0,1);",
                "timeout": ("const until=performance.now()+350;"
                            "while(performance.now()<until){}"),
            }[delimiter_mode]
            delimiter_script = (
                'a.addEventListener("keydown",e=>{'
                'if(e.ctrlKey&&e.shiftKey&&e.key==="ArrowLeft"){'
                'nativeUnits++;if(nativeUnits===2){' + action + '}}});')
            delimiter_state = "nativeUnits,"
        page.write_text(
            '<!doctype html><meta charset="utf-8"><title>starting</title>'
            '<style>input,textarea{display:block;margin:20px;width:400px;height:40px;'
            'font-size:24px}</style>' + first_control +
            f'<input id="b" value="{second_value}"><script>'
            'let keys=0,snapshots=0,tabsDown=0,tabsUp=0,nativeUnits=0;' +
            delimiter_script + rollback_script + cleanup_script +
            'addEventListener("keydown",e=>{keys++;if(e.key==="Tab")tabsDown++});'
            'addEventListener("keyup",e=>{if(e.key==="Tab")tabsUp++});'
            'setInterval(()=>document.title="PUNTO"+encodeURIComponent(JSON.stringify('
            '[a.value,a.selectionStart,a.selectionEnd,b.value,b.selectionStart,'
            'b.selectionEnd,document.activeElement.id,keys,'
            '[b.getBoundingClientRect().left,b.getBoundingClientRect().top,'
            'outerHeight-innerHeight],[tabsDown,tabsUp],' + delimiter_state + rollback_state + cleanup_state +
            '++snapshots])),20)</script>',
            encoding="utf-8",
        )
        self.browser_log = tempfile.TemporaryFile()
        self.addCleanup(self.browser_log.close)
        self.browser = subprocess.Popen(
            [browser_path(), "--no-sandbox", "--disable-dev-shm-usage",
             "--disable-gpu", "--no-first-run", "--no-default-browser-check",
             "--disable-background-networking", "--disable-component-update",
             "--disable-sync", "--password-store=basic", "--ozone-platform=x11",
             f"--user-data-dir={self.profile}/profile", f"--app={page.as_uri()}"],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=self.browser_log, start_new_session=True,
        )
        self.browser_window = None

        def find_window():
            self.assertIsNone(self.browser.poll(), "private Chromium exited")
            result = subprocess.run(
                ["xdotool", "search", "--name", r"^PUNTO%5B"],
                capture_output=True, text=True, timeout=2,
            )
            windows = result.stdout.splitlines()
            if windows:
                self.browser_window = int(windows[0])
            return self.browser_window is not None

        self.pump_until(find_window, "private Chromium window", timeout=45)
        self.xdo("windowfocus", "--sync", str(self.browser_window))
        self.publish_active_window(self.browser_window)
        self.pump_until(lambda: self.browser_state()[6] == "a", "browser input focus")

    def stop_browser(self):
        for action in (signal.SIGTERM, signal.SIGKILL):
            try:
                os.killpg(self.browser.pid, action)
            except ProcessLookupError:
                pass
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                if (self.browser.poll() is not None and
                        not browser_group_has_live_members(self.browser.pid)):
                    return
                time.sleep(.01)
        raise RuntimeError("private browser process group did not stop before profile cleanup")

    def cleanup_browser(self):
        if self.browser is not None:
            self.stop_browser()
        shutil.rmtree(self.profile)

    def browser_state(self):
        title = self.xdo("getwindowname", str(self.browser_window))
        self.assertTrue(title.startswith("PUNTO%5B"), title)
        return json.loads(urllib.parse.unquote(title[5:]))

    def wait_for_fresh_dom(self, serial, description):
        self.pump_until(lambda: self.browser_state()[-1] > serial, description)

    def dispatches(self):
        return int(self.stats_fields()[1]["word_dispatches"])

    def full_diagnostic(self):
        return bytes(self.harness.stderr).decode("utf-8", errors="replace")

    def tearDown(self):
        try:
            if os.environ.get("PUNTO_EVENT_LOOP_E2E_ORIGIN_TRACE") == "1":
                origin_lines = [line for line in self.full_diagnostic().splitlines()
                                if line.startswith("[punto-e2e] origin_")]
                print("\n".join(origin_lines[-64:]), file=sys.stderr)
            result = self._outcome.result
            for case, traceback in (*result.failures, *result.errors):
                if case is self or getattr(case, "test_case", None) is self:
                    print(f"[punto-e2e] failed case {case.id()}\n{traceback}", file=sys.stderr)
                    print(self.full_diagnostic(), file=sys.stderr)
        finally:
            super().tearDown()

    def word_edit_attempts(self):
        return self.full_diagnostic().count("Word edit dispatch status=")

    def clipboard_owner(self):
        return self.selection_owner("CLIPBOARD")

    def rich_clipboard_snapshot(self):
        targets = subprocess.run(
            ["xclip", "-selection", "clipboard", "-t", "TARGETS", "-o"],
            check=True, capture_output=True, text=True, timeout=3,
        ).stdout.splitlines()
        checked_targets = sorted(
            target for target in targets
            if target == "text/html" or target.startswith("chromium/x-")
        )
        payloads = {
            target: subprocess.run(
                ["xclip", "-selection", "clipboard", "-t", target, "-o"],
                check=True, capture_output=True, timeout=3,
            ).stdout
            for target in checked_targets
        }
        return self.clipboard_owner(), sorted(targets), payloads

    def external_clipboard_target(self, target):
        # Pump GTK while an independent X client reads a GTK-owned selection.
        reader = subprocess.Popen(
            ["xclip", "-selection", "clipboard", "-t", target, "-o"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        self.addCleanup(reader.kill if reader.poll() is None else lambda: None)
        self.pump_until(lambda: reader.poll() is not None,
                        f"external clipboard {target} read")
        stdout, stderr = reader.communicate(timeout=1)
        self.assertEqual(reader.returncode, 0, stderr.decode(errors="replace"))
        return stdout

    def plain_clipboard_snapshot(self):
        return (
            self.clipboard_owner(),
            sorted(self.external_clipboard_target("TARGETS").decode().splitlines()),
            self.external_clipboard_target("UTF8_STRING"),
        )

    def arm_held_space(self):
        # Only this private server's Space repeat is disabled for held-key tests.
        repeat_command = ["xset", "-display", self.x11.display]
        subprocess.run([*repeat_command, "-r", str(gtk.KEY_SPACE + 8)],
                       check=True, timeout=3)
        self.addCleanup(subprocess.run,
                        [*repeat_command, "r", str(gtk.KEY_SPACE + 8)],
                        check=True, timeout=3)
        self.harness.relay.arm_delayed_key_release(gtk.KEY_SPACE)
        self.addCleanup(self.harness.relay.permit_key_release.set)
        marker = pathlib.Path("/run/punto-e2e-arm-key-release-check")
        checked = pathlib.Path("/run/punto-e2e-key-release-checked")
        checked.unlink(missing_ok=True)
        marker.touch(mode=0o600)
        self.addCleanup(marker.unlink, missing_ok=True)
        self.addCleanup(checked.unlink, missing_ok=True)
        self.assertEqual(gtk.ipc_request(b"SET_STATUS 1\n"), b"OK ENABLED\n")
        self.harness.type_word("ghbdtn")
        self.harness.send_key(gtk.KEY_SPACE)
        self.pump_until(self.harness.relay.key_release_blocked.is_set,
                        "Space release held in the private relay")
        self.assertTrue(self.key_is_down(gtk.KEY_SPACE))
        return checked

    def assert_held_space_rejected(self):
        self.pump_until(lambda: "Word edit dispatch status=0" in self.harness.diagnostic(),
                        "held Space rejected before any edit", timeout=1)
        self.assertTrue(self.key_is_down(gtk.KEY_SPACE), "Punto must not force keyup")
        self.assertFalse(self.harness.relay.key_release_delivered.is_set())
        self.assertEqual(self.dispatches(), 0)
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn ", 7, 7],
                        "source text and caret preserved while Space remains down")

    def release_held_space(self):
        self.harness.relay.permit_key_release.set()
        self.pump_until(self.harness.relay.key_release_delivered.is_set,
                        "actual queued Space release delivered")
        self.pump_until(lambda: not self.key_is_down(gtk.KEY_SPACE), "Space is up")
        serial = self.browser_state()[-1]
        self.pump_until(lambda: self.browser_state()[-1] > serial,
                        "fresh DOM after actual Space release")
        self.assertEqual(self.browser_state()[:3], ["ghbdtn ", 7, 7])

    def first_manual_conversion(self):
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "browser typed source")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == ["привет", 6, 6],
                        "first native browser correction")
        self.pump_until(lambda: self.dispatches() == 1, "first dispatch receipt")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_PRIMARY), "ghbdtn")

    def assert_pause_rejected(self, *, preflight=False,
                              expected_stage=None):
        before = self.browser_state()
        dispatches = self.dispatches()
        status_line = "Word edit dispatch status=0"
        attempts = self.word_edit_attempts()
        rejected = self.full_diagnostic().count(status_line)
        stages = ((expected_stage,) if isinstance(expected_stage, str)
                  else (expected_stage or ()))
        stage_count = sum(self.full_diagnostic().count(f"rejection_stage={stage}")
                          for stage in stages)
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.word_edit_attempts() > attempts,
            "browser word-edit attempt",
        )
        diagnostic = self.full_diagnostic()
        self.assertGreater(diagnostic.count(status_line), rejected, diagnostic)
        if expected_stage is not None:
            self.assertGreater(
                sum(diagnostic.count(f"rejection_stage={stage}")
                    for stage in stages), stage_count, diagnostic)
        serial = self.browser_state()[-1]
        self.pump_until(lambda: self.browser_state()[-1] > serial,
                        "fresh DOM snapshot after rejection")
        after = self.browser_state()
        if preflight:
            self.assertEqual(after[:-1], before[:-1])
        else:
            self.assertEqual(after[:7], before[:7])
            self.assertEqual(after[8], before[8])
        self.assertEqual(self.dispatches(), dispatches)

    def select_field_b_word(self, delay=100):
        left, top, decoration = self.browser_state()[8]
        self.xdo("mousemove", "--window", str(self.browser_window),
                 str(int(left + 30)), str(int(top + decoration + 20)))
        self.xdo("click", "--repeat", "2", "--delay", str(delay), "1")
        self.pump_until(
            lambda: self.browser_state()[3:7] == ["ghbdtn", 0, 6, "b"],
            "browser field b word selection",
        )

    def assert_fresh_physical_manual_recovery(self, authority):
        daemon_pid, daemon_start, clipboard_owner, clipboard = authority
        dispatches = self.dispatches()
        following = self.browser_state()[3:6]
        source_group = self.keyboard_group()
        self.assertIn(source_group, (0, 1))
        self.assertEqual(self.desktop_source_role(), source_group)
        source = ("ghbdtn", "привет")[source_group]
        target_group = 1 - source_group
        replacement = ("ghbdtn", "привет")[target_group]
        next_glyph = ("a", "ф")[target_group]
        modifier, key = ((gtk.KEY_LEFTCTRL, gtk.KEY_GRAVE)
                         if self.real_layout_bridge is not None else
                         (self.layout_shortcut.modifier, self.layout_shortcut.key))
        self.send_chord((gtk.KEY_LEFTCTRL,), key=gtk.LETTER_CODES["a"])
        self.harness.send_key(gtk.KEY_BACKSPACE)
        self.pump_until(lambda: self.browser_state()[:3] == ["", 0, 0],
                        "physical clear after persistent manual refusal")
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == [source, 6, 6],
                        "fresh physical source after persistent refusal")
        resets = self.full_diagnostic().count("[punto-e2e] origin_reset ")
        self.harness.send_key(gtk.KEY_RIGHT)
        self.pump_until(lambda: self.full_diagnostic().count(
            "[punto-e2e] origin_reset ") > resets,
                        "ordinary Right clears fresh first-key provenance")
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after recovery navigation")
        self.assertEqual(self.browser_state()[:3], [source, 6, 6])
        self.assertEqual(self.stats_fields()[1]["history_words"], "0")
        self.assertEqual(self.dispatches(), dispatches)
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == [replacement, 6, 6],
                        "new manual preparation after fresh physical text")
        self.pump_until(lambda: self.dispatches() == dispatches + 1,
                        "one fresh recovery dispatch")
        self.assertEqual(self.keyboard_group(), target_group)
        self.assertEqual(self.desktop_source_role(), target_group)
        self.harness.type_word("a")
        self.pump_until(lambda: self.browser_state()[:3] ==
                        [replacement + next_glyph, 7, 7],
                        "recovery next glyph follows actual target source")
        self.assertEqual(self.browser_state()[3:6], following)
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD), clipboard)
        self.assertEqual(self.clipboard_owner(), clipboard_owner)
        self.assertEqual(self.output_count(gtk.KEY_PAUSE), 0)
        for key in (gtk.KEY_PAUSE, gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT,
                    gtk.KEY_LEFTALT, gtk.KEY_RIGHT, gtk.KEY_BACKSPACE,
                    key, modifier):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(gtk.process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())

    def assert_stale_primary_refuses_then_fresh_word_recovers(self):
        daemon_pid = self.harness.process.pid
        daemon_start = gtk.process_start_time(daemon_pid)
        clipboard = self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD)
        self.assert_pause_rejected(preflight=True)
        dispatches = self.dispatches()
        if self.keyboard_group() != 0:
            self.send_chord((self.layout_shortcut.modifier,),
                            key=self.layout_shortcut.key)
            self.pump_until(lambda: self.keyboard_group() == 0,
                            "English layout before stale PRIMARY control")
        self.harness.send_key(gtk.KEY_RIGHT)
        self.pump_until(
            lambda: self.browser_state()[1:3] == [6, 6],
            "collapse prepared selection before stale PRIMARY control",
        )
        self.harness.send_key(gtk.KEY_SPACE)
        self.harness.type_word("ghbdtn")
        self.pump_until(
            lambda: self.browser_state()[:3] == ["привет ghbdtn", 13, 13],
            "fresh word after invalidated prepared receipt",
        )
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == ["привет привет", 13, 13],
                        "new physical origin admits a new exact word range")
        self.assertEqual(self.dispatches(), dispatches + 1)
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[:3] == ["привет привета", 14, 14],
                        "next physical key follows the recovered RU source")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(gtk.process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD), clipboard)
        for key in (gtk.KEY_PAUSE, gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT, gtk.KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def test_repeated_manual_conversion_with_retained_primary(self):
        self.first_manual_conversion()
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "second native browser correction with retained PRIMARY")
        self.pump_until(lambda: self.dispatches() == 2, "second dispatch receipt")

    def test_selection_hotkeys_without_visible_selection_preserve_receipt(self):
        self.first_manual_conversion()
        clipboard = self.plain_clipboard_snapshot()
        daemon_pid = self.harness.process.pid
        for modifiers in ((gtk.KEY_LEFTSHIFT,), (gtk.KEY_LEFTALT,),
                          (gtk.KEY_LEFTCTRL, gtk.KEY_LEFTALT)):
            with self.subTest(modifiers=modifiers):
                rejected = self.full_diagnostic().count("rejection_stage=selection_range_empty")
                self.send_chord(modifiers)
                self.pump_until(
                    lambda: self.full_diagnostic().count(
                        "rejection_stage=selection_range_empty") > rejected,
                    "selection-only operation rejects retained PRIMARY at collapsed caret",
                )
                self.wait_for_fresh_dom(self.browser_state()[-1], "fresh DOM after refusal")
                self.assertEqual(self.browser_state()[:3], ["привет", 6, 6])
                self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_PRIMARY), "ghbdtn")
                self.assertEqual(self.dispatches(), 1)
                self.assertEqual(self.keyboard_group(), 1)
                self.assertEqual(self.plain_clipboard_snapshot(), clipboard)
                for key in (*modifiers, gtk.KEY_PAUSE):
                    self.assertFalse(self.key_is_down(key))
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "safe selection refusal preserves ordinary word reversal")
        self.pump_until(lambda: self.dispatches() == 2, "word reversal dispatched")
        self.xdo("key", "ctrl+a")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 0, 6],
                        "fresh real selection after refusal and word recovery")
        self.send_chord((gtk.KEY_LEFTSHIFT,))
        self.pump_until(lambda: self.browser_state()[:3] == ["привет", 6, 6],
                        "fresh selection remains eligible without restart")
        self.pump_until(lambda: self.dispatches() == 3, "fresh selection dispatched")
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[:3] == ["привета", 7, 7],
                        "next ordinary symbol follows the selected target layout")
        self.assertEqual(self.plain_clipboard_snapshot(), clipboard)
        self.assertEqual(self.output_count(gtk.KEY_PAUSE), 0)
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())

    def assert_first_word_in_another_field(self, *, physical_tab=False):
        self.harness.type_word("hELLo")
        self.pump_until(lambda: self.browser_state()[:3] == ["hELLo", 5, 5],
                        "first field physical word")
        self.send_chord((gtk.KEY_LEFTCTRL,))
        self.pump_until(lambda: self.browser_state()[:3] == ["HellO", 5, 5],
                        "first field case correction")
        self.pump_until(lambda: self.dispatches() == 1, "first field dispatch")
        # Move the real browser's focus without telling the daemon to clear its
        # window-scoped history. Both DOM controls belong to the same X11 window.
        if physical_tab:
            self.harness.send_key(gtk.KEY_TAB)
        else:
            self.xdo("key", "Tab")
        self.pump_until(lambda: self.browser_state()[3:7] == ["", 0, 0, "b"],
                        "second browser field focused")
        self.harness.type_word("WorLD")
        self.pump_until(lambda: self.browser_state()[3:7] == ["WorLD", 5, 5, "b"],
                        "different first word in second field")
        self.send_chord((gtk.KEY_LEFTCTRL,))
        self.pump_until(lambda: self.browser_state()[:7] ==
                        ["HellO", 5, 5, "wORld", 5, 5, "b"],
                        "only second field's current word corrected")
        self.pump_until(lambda: self.dispatches() == 2, "second field dispatch")
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[:7] ==
                        ["HellO", 5, 5, "wORldf", 6, 6, "b"],
                        "next ordinary key in second field")
        self.assertEqual(self.keyboard_group(), 0)
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.assertEqual(self.output_count(gtk.KEY_PAUSE), 0)
        for key in (gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT, gtk.KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def test_extensionless_first_word_in_another_field(self):
        self.assert_first_word_in_another_field()

    def test_extensionless_physical_tab_preserves_next_field_correction(self):
        self.assert_first_word_in_another_field(physical_tab=True)
        self.assertEqual(self.output_count(gtk.KEY_TAB, 1), 1)
        self.assertEqual(self.output_count(gtk.KEY_TAB, 0), 1)
        self.assertEqual(self.browser_state()[9], [1, 1])

    def assert_native_current_word_case(self, source, expected, caret=None):
        # No daemon history: the actual application's range is the authority.
        self.xdo("type", "--clearmodifiers", "--delay", "0", source)
        self.pump_until(lambda: self.browser_state()[:3] ==
                        [source, len(source), len(source)],
                        "pre-existing application text")
        position = len(source) if caret is None else caret
        if caret is not None:
            self.xdo("key", "Home")
            for _ in range(caret):
                self.xdo("key", "Right")
            self.pump_until(lambda: self.browser_state()[1:3] == [caret, caret],
                            "mid-word application caret")
        self.send_chord((gtk.KEY_LEFTCTRL,))
        self.pump_until(lambda: self.browser_state()[:3] ==
                        [expected, position, position],
                        "exact application-word case range")
        self.pump_until(lambda: self.dispatches() == 1, "current word dispatched")
        self.harness.type_word("f")
        following = expected[:position] + "f" + expected[position:]
        self.pump_until(lambda: self.browser_state()[:3] ==
                        [following, position + 1, position + 1],
                        "next key preserves unselected prefix and suffix")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.assertNotIn("[punto-e2e] clipboard_publish", self.full_diagnostic())
        self.assertEqual(self.output_count(gtk.KEY_PAUSE), 0)
        for key in (gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT):
            self.assertFalse(self.key_is_down(key))

    def test_extensionless_current_word_preserves_prefix(self):
        self.assert_native_current_word_case("prev hELLo", "prev HellO")

    def test_extensionless_current_word_preserves_punctuation(self):
        self.assert_native_current_word_case("prev hELLo.", "prev HellO.")

    def test_extensionless_current_word_preserves_trailing_spaces(self):
        self.assert_native_current_word_case("prev hELLo  ", "prev HellO  ")

    def test_extensionless_current_word_at_midword_caret(self):
        self.assert_native_current_word_case("prev hELLo", "prev HelLo", caret=8)

    def assert_layout_follows_actual_source(self, source, desktop_group,
                                          expected, target_group, *, selected=False):
        self.xdo("key", "End")
        caret = len(source.encode("utf-16-le")) // 2
        self.pump_until(lambda: self.browser_state()[:3] == [source, caret, caret],
                        "pre-existing actual word without daemon history")
        self.set_desktop_layout(desktop_group)
        self.assertEqual(self.keyboard_group(), desktop_group)
        self.assertEqual(self.desktop_source_role(), desktop_group)
        if selected:
            self.xdo("key", "ctrl+a")
            self.pump_until(lambda: self.browser_state()[:3] == [source, 0, caret],
                            "exact user selection with mismatched desktop layout")
            self.send_chord((gtk.KEY_LEFTSHIFT,))
        else:
            self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == [expected, caret, caret],
                        "actual source determines the conversion direction")
        self.pump_until(lambda: self.dispatches() == 1, "actual-source dispatch")
        self.assertEqual(self.keyboard_group(), target_group)
        self.assertEqual(self.desktop_source_role(), target_group)
        self.pump_until(lambda: self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline", "plain clipboard restored")
        self.harness.type_word("f")
        following = expected + ("а" if target_group else "f")
        self.pump_until(lambda: self.browser_state()[:3] ==
                        [following, caret + 1, caret + 1],
                        "next physical key follows the transformed word's layout")
        self.assertEqual(self.browser_state()[3:7], ["ghbdtn", 0, 0, "a"])
        self.assertEqual(self.output_count(gtk.KEY_PAUSE), 0)
        for key in (gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT, gtk.KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def test_extensionless_current_ru_word_with_en_desktop(self):
        self.assert_layout_follows_actual_source("привет", 0, "ghbdtn", 0)

    def test_extensionless_current_en_word_with_ru_desktop(self):
        self.assert_layout_follows_actual_source("ghbdtn", 1, "привет", 1)

    def test_extensionless_selected_ru_word_with_en_desktop(self):
        self.assert_layout_follows_actual_source("привет", 0, "ghbdtn", 0, selected=True)

    def test_extensionless_selected_en_word_with_ru_desktop(self):
        self.assert_layout_follows_actual_source("ghbdtn", 1, "привет", 1, selected=True)

    def assert_inconsistent_emoji_selection_refuses_and_recovers(self, source):
        self.xdo("key", "End", "ctrl+a")
        self.pump_until(lambda: self.browser_state()[:3] == [source, 0, 8],
                        "actual selected emoji source before provider observation")
        clipboard = self.plain_clipboard_snapshot()
        rejected = self.full_diagnostic().count("rejection_stage=selection_range_empty")
        self.send_chord((gtk.KEY_LEFTSHIFT,))
        self.pump_until(lambda: self.full_diagnostic().count(
            "rejection_stage=selection_range_empty") > rejected,
            "inconsistent provider range refuses rather than trusting PRIMARY")
        self.wait_for_fresh_dom(self.browser_state()[-1], "fresh DOM after provider refusal")
        self.assertEqual(self.browser_state()[:3], [source, 0, 8])
        self.assertEqual(self.dispatches(), 0)
        self.assertEqual(self.keyboard_group(), 0)
        self.assertEqual(self.plain_clipboard_snapshot(), clipboard)
        self.xdo("type", "--clearmodifiers", "ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "fresh representable source replaces rejected selection")
        self.xdo("key", "ctrl+a")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 0, 6],
                        "fresh valid provider range")
        self.send_chord((gtk.KEY_LEFTSHIFT,))
        self.pump_until(lambda: self.browser_state()[:3] == ["привет", 6, 6],
                        "valid fresh selection recovers without restart")
        self.pump_until(lambda: self.dispatches() == 1, "valid recovery dispatched")
        self.assertEqual(self.plain_clipboard_snapshot(), clipboard)
        self.assertEqual(self.output_count(gtk.KEY_PAUSE), 0)
        for key in (gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT, gtk.KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def test_extensionless_selected_ru_emoji_with_en_desktop(self):
        self.assert_inconsistent_emoji_selection_refuses_and_recovers("привет🙂")

    def test_extensionless_emoji_selection_refuses_and_recovers(self):
        self.assert_inconsistent_emoji_selection_refuses_and_recovers("ghbdtn🙂")

    def test_selection_paste_accepts_long_utf8_source_range(self):
        source = "ф" * 550 + "☃☃"
        replacement = "a" * 550 + "☃☃"
        self.assertGreater(len(source.encode("utf-8")), 1024)
        self.set_selection(gtk.Gdk.SELECTION_CLIPBOARD, source)
        self.xdo("key", "ctrl+v")
        self.pump_until(lambda: self.browser_state()[:3] == [source, len(source), len(source)],
                        "long Unicode source pasted without correction history")
        self.set_selection(gtk.Gdk.SELECTION_CLIPBOARD, "startup clipboard baseline")
        clipboard = self.plain_clipboard_snapshot()
        self.xdo("key", "ctrl+a")
        self.pump_until(lambda: self.browser_state()[:3] == [source, 0, len(source)],
                        "long selection exceeds old character and byte proof limits")
        self.send_chord((gtk.KEY_LEFTSHIFT,))
        self.pump_until(lambda: self.browser_state()[:3] ==
                        [replacement, len(replacement), len(replacement)],
                        "long selected source uses guarded Unicode paste")
        self.pump_until(lambda: self.dispatches() == 1, "large selected operation dispatched")
        self.pump_until(lambda: self.external_clipboard_target("UTF8_STRING") == clipboard[2],
                        "large selected operation restores clipboard text")
        restored = self.plain_clipboard_snapshot()
        metadata = {"TARGETS", "TIMESTAMP", "MULTIPLE", "SAVE_TARGETS"}
        self.assertEqual(set(restored[1]) - metadata, set(clipboard[1]) - metadata)
        self.assertEqual(restored[2], clipboard[2])
        self.assertEqual(self.keyboard_group(), 0)
        self.assertEqual(self.output_count(gtk.KEY_PAUSE), 0)

    def test_extensionless_layout_ack_failure_allows_immediate_retry(self):
        daemon_pid = self.harness.process.pid
        fault = pathlib.Path("/run/punto-e2e-fail-layout-hotkey-send")
        ack = pathlib.Path("/run/punto-e2e-layout-hotkey-ack-fault-consumed")
        for marker in (fault, ack):
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "source before consumed layout acknowledgment failure")
        attempts = self.word_edit_attempts()
        fault.touch(mode=0o600)
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.word_edit_attempts() > attempts,
                        "failed acknowledgment completed without text replay")
        self.assertFalse(fault.exists())
        self.assertTrue(ack.exists())
        self.wait_for_fresh_dom(self.browser_state()[-1], "fresh failure DOM")
        self.assertEqual(self.browser_state()[:3], ["ghbdtn", 6, 6])
        self.assertEqual(self.dispatches(), 0)
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.desktop_source_role(), 1)
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == ["привет", 6, 6],
                        "immediate fresh retry without typing or manual layout change")
        self.pump_until(lambda: self.dispatches() == 1, "fresh retry dispatched once")
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.desktop_source_role(), 1)
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[:3] == ["привета", 7, 7],
                        "retry establishes Russian for the next ordinary key")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())
        self.assertEqual(self.output_count(gtk.KEY_PAUSE), 0)
        for key in (gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT, gtk.KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def test_extensionless_whitespace_refusal_recovers(self):
        self.xdo("type", "--clearmodifiers", "  ")
        self.pump_until(lambda: self.browser_state()[:3] == ["  ", 2, 2],
                        "whitespace-only application range")
        attempts = self.word_edit_attempts()
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.word_edit_attempts() > attempts,
                        "whitespace correction was attempted")
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "whitespace refusal DOM")
        self.assertEqual(self.browser_state()[:3], ["  ", 2, 2])
        self.assertEqual(self.dispatches(), 0)
        self.harness.type_word("ghbdtn")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == ["  привет", 8, 8],
                        "fresh word recovers after whitespace refusal")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")

    def test_extensionless_current_word_uses_actual_literal_tab(self):
        self.xdo("key", "End")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn\t", 7, 7],
                        "application-only literal tab without daemon history")
        for count, expected in enumerate(("привет\t", "ghbdtn\t", "привет\t"), 1):
            self.harness.send_key(gtk.KEY_PAUSE)
            self.pump_until(lambda expected=expected: self.browser_state()[:3] ==
                            [expected, 7, 7], "literal tab remains text on each correction")
            self.pump_until(lambda count=count: self.dispatches() == count,
                            "fresh repeated literal-tab operation")
            self.assertEqual(self.browser_state()[6], "a")
            self.assertEqual(self.browser_state()[9], [0, 0])
            try:
                self.pump_until(lambda: self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD)
                                == "startup clipboard baseline",
                                "literal-tab paste restores previous plain text", timeout=2)
            except self.failureException:
                print(self.full_diagnostic(), file=sys.stderr)
                print("clipboard-owner-after-failure=" +
                      str(self.selection_owner("CLIPBOARD")), file=sys.stderr)
                raise
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[:3] == ["привет\tа", 8, 8],
                        "next ordinary key remains in the textarea and Russian layout")
        self.assertEqual(self.browser_state()[3:7], ["ghbdtn", 0, 0, "a"])
        self.assertEqual(self.dispatches(), 3)
        for key in (gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT):
            self.assertFalse(self.key_is_down(key))

    def prepare_unrepresentable_selection(self):
        # Bound growth without invalidating ASan's existing shadow mappings.
        budget = 512 * 1024 * 1024
        mapped = int(pathlib.Path(f"/proc/{self.harness.process.pid}/statm")
                     .read_text(encoding="ascii").split()[0]) * os.sysconf("SC_PAGE_SIZE")
        limit = mapped + budget
        resource.prlimit(self.harness.process.pid, resource.RLIMIT_AS, (limit, limit))
        self.xdo("key", "End")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn☃☃", 8, 8],
                        "source with a character outside the keyboard map")
        self.xdo("key", "ctrl+a")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn☃☃", 0, 8],
                        "exact application selection outside the keyboard map")

    def test_extensionless_unrepresentable_selection_restores_plain_clipboard(self):
        self.prepare_unrepresentable_selection()
        self.send_chord((gtk.KEY_LEFTSHIFT,))
        self.pump_until(lambda: self.browser_state()[:3] == ["привет☃☃", 8, 8],
                        "unrepresentable selection uses guarded clipboard transfer")
        self.pump_until(lambda: self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline",
                        "paste restores previous plain text", timeout=2)
        self.xdo("key", "ctrl+a")
        self.pump_until(lambda: self.browser_state()[:3] == ["привет☃☃", 0, 8],
                        "fresh selection for repeated clipboard conversion")
        self.send_chord((gtk.KEY_LEFTSHIFT,))
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn☃☃", 8, 8],
                        "fresh repeated operation after clipboard restoration")
        self.pump_until(lambda: self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline",
                        "repeated paste also restores plain text", timeout=2)
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn☃☃f", 9, 9],
                        "ordinary key after repeated clipboard conversion")
        self.assertEqual(self.dispatches(), 2)

    def real_layout_bridge_paste_retains_payload_during_layout_settle(self):
        self.prepare_unrepresentable_selection()
        daemon_pid = self.harness.process.pid
        daemon_start = gtk.process_start_time(daemon_pid)
        attempts = self.word_edit_attempts()
        activations = self.desktop_activation_count()
        self.assertIsNotNone(self.real_layout_bridge)
        before = self.browser_state()
        self.assertEqual(before[-2], [0, 0])
        marker_names = (
            "punto-e2e-arm-after-paste-delivered", "punto-e2e-after-paste-delivered",
            "punto-e2e-release-after-paste-delivered",
        )
        markers = {name: pathlib.Path("/run", name) for name in marker_names}
        for marker in markers.values():
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        release = markers["punto-e2e-release-after-paste-delivered"]
        self.addCleanup(release.touch, mode=0o600, exist_ok=True)
        markers["punto-e2e-arm-after-paste-delivered"].touch(mode=0o600)
        self.send_chord((gtk.KEY_LEFTSHIFT,))
        self.pump_until(markers["punto-e2e-after-paste-delivered"].exists,
                        "real delivered paste entered the bounded pumping seam")
        self.assertFalse(markers["punto-e2e-arm-after-paste-delivered"].exists())
        self.pump_until(lambda: self.browser_state()[-1] > before[-1]
                        and self.browser_state()[:3] == ["привет☃☃", 8, 8]
                        and self.browser_state()[-2] == [1, 1],
                        "fresh DOM proves the actual paste event and exact replacement")
        self.assertEqual(self.desktop_activation_count(), activations + 1)
        self.assertEqual(self.keyboard_group(), 1)
        payload_owner = self.clipboard_owner()
        self.assertNotEqual(payload_owner, 0)
        self.pump_for(.35)
        self.assertEqual(self.external_selection_text(), "привет☃☃",
                         "macro still owns its delivered paste payload after quiet budget")
        self.assertEqual(self.clipboard_owner(), payload_owner)
        self.assertEqual(self.word_edit_attempts(), attempts,
                         "clipboard was served before macro completion: " +
                         self.full_diagnostic())
        release.touch(mode=0o600)
        self.pump_until(lambda: not release.exists(), "delivered-paste seam release consumed")
        self.pump_until(lambda: self.word_edit_attempts() > attempts,
                        "released macro reached its terminal dispatch")
        self.pump_until(lambda: self.browser_state()[:3] == ["привет☃☃", 8, 8],
                        "paste completes with the transformed source")
        self.pump_until(lambda: self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD)
                        == "startup clipboard baseline",
                        "completed macro restores the previous plain text", timeout=2)
        self.assertEqual(self.dispatches(), 1, self.full_diagnostic())
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.desktop_activation_count(), activations + 1)
        self.assertEqual(self.browser_state()[3:7], before[3:7])
        self.assertEqual(self.browser_state()[-2], [1, 1])
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[:3] == ["привет☃☃а", 9, 9],
                        "next physical glyph uses the completed RU source")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        for key in (gtk.KEY_PAUSE, gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT, gtk.KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(gtk.process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())

    def assert_unsent_paste_rollback(self, boundary, stage, *, foreign_clipboard=False):
        self.prepare_unrepresentable_selection()
        daemon_pid = self.harness.process.pid
        source = self.browser_state()[:7]
        pointer = self.pointer_position()
        self.addCleanup(self.xdo, "mousemove", str(pointer[0]), str(pointer[1]))
        marker_names = (
            f"punto-e2e-arm-{boundary}", f"punto-e2e-{boundary}",
            f"punto-e2e-release-{boundary}",
            "punto-e2e-drop-rollback-timestamp", "punto-e2e-rollback-timestamp-dropped",
        )
        markers = {name: pathlib.Path("/run", name) for name in marker_names}
        for marker in markers.values():
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        release = markers[f"punto-e2e-release-{boundary}"]
        self.addCleanup(release.touch, mode=0o600, exist_ok=True)
        attempts = self.word_edit_attempts()
        markers[f"punto-e2e-arm-{boundary}"].touch(mode=0o600)
        self.send_chord((gtk.KEY_LEFTSHIFT,))
        self.pump_until(markers[f"punto-e2e-{boundary}"].exists,
                        "real temporary clipboard publication reached the bounded seam")
        self.assertFalse(markers[f"punto-e2e-arm-{boundary}"].exists())

        if foreign_clipboard:
            baseline = "foreign clipboard Ω"
            self.set_selection(gtk.Gdk.SELECTION_CLIPBOARD, baseline)
            foreign_snapshot = self.plain_clipboard_snapshot()
        else:
            baseline = "startup clipboard baseline"
            markers["punto-e2e-drop-rollback-timestamp"].touch(mode=0o600)
        if boundary == "before-paste-receipt":
            self.set_selection(gtk.Gdk.SELECTION_PRIMARY, "foreign PRIMARY")
        elif boundary == "after-paste-receipt":
            self.xdo("mousemove", str(pointer[0] + 40), str(pointer[1]))
        release.touch(mode=0o600)
        self.pump_until(lambda: self.word_edit_attempts() > attempts,
                        "real unsent paste branch returned a terminal outcome")
        self.assertIn(f"rejection_stage={stage}", self.full_diagnostic())
        self.wait_for_fresh_dom(self.browser_state()[-1], "fresh refused-paste DOM")
        self.assertEqual(self.browser_state()[:7], source)
        self.assertEqual(self.browser_state()[-2], [0, 0],
                         "neither a browser paste event nor Ctrl+V was delivered")
        self.assertEqual(self.dispatches(), 0)
        if foreign_clipboard:
            self.wait_event_loop_idle("foreign clipboard cancellation processed")
            self.pump_for(.3)
            self.assertEqual(self.plain_clipboard_snapshot(), foreign_snapshot,
                             "deferred rollback never overwrites a new owner or payload")
        else:
            self.pump_until(markers["punto-e2e-rollback-timestamp-dropped"].exists,
                            "one actual rollback PropertyNotify was withheld")
            self.assertFalse(markers["punto-e2e-drop-rollback-timestamp"].exists())
            self.pump_until(lambda: self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD)
                            == baseline,
                            "transient rollback failure retains and later restores baseline",
                            timeout=2)
        if boundary == "before-paste-receipt":
            self.assertEqual(self.external_selection_text("primary"), "foreign PRIMARY",
                             "clipboard rollback has no foreign-selection cleanup authority")
        for key in (gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT):
            self.assertFalse(self.key_is_down(key))

        self.xdo("mousemove", str(pointer[0]), str(pointer[1]))
        self.xdo("key", "End", "ctrl+a")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn☃☃", 0, 8],
                        "fresh application selection after refusal")
        self.assertEqual(self.external_selection_text("primary"), "ghbdtn☃☃")
        self.send_chord((gtk.KEY_LEFTSHIFT,))
        self.pump_until(lambda: self.browser_state()[:3] == ["привет☃☃", 8, 8],
                        "fresh correction recovers without restart")
        self.pump_until(lambda: self.external_selection_text() == baseline,
                        "recovered paste restores its admitted plain-text baseline", timeout=2)
        self.assertEqual(self.browser_state()[-2], [1, 1])
        self.assertEqual(self.dispatches(), 1)
        self.assertEqual(self.keyboard_group(), 1)
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[:3] == ["привет☃☃а", 9, 9],
                        "next ordinary character uses the recovered RU source")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())
        for key in (gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT):
            self.assertFalse(self.key_is_down(key))

    def test_extensionless_unsent_receipt_refusal_restores_and_recovers(self):
        self.assert_unsent_paste_rollback("before-paste-receipt", "paste_receipt_arm")

    def test_extensionless_unsent_context_refusal_restores_and_recovers(self):
        self.assert_unsent_paste_rollback("after-paste-receipt", "paste_context_before_dispatch")

    def test_extensionless_unsent_chord_expiry_restores_and_recovers(self):
        try:
            self.assert_unsent_paste_rollback("before-paste-chord", "paste_submission")
        except self.failureException:
            print(self.full_diagnostic(), file=sys.stderr)
            raise

    def test_extensionless_unsent_rollback_preserves_foreign_clipboard(self):
        self.assert_unsent_paste_rollback("after-paste-receipt", "paste_context_before_dispatch",
                                        foreign_clipboard=True)

    def test_extensionless_unrepresentable_selection_preserves_rich_clipboard(self):
        self.prepare_unrepresentable_selection()
        self.xdo("key", "ctrl+c")
        before = self.rich_clipboard_snapshot()
        self.assertIn("chromium/x-internal-source-rfh-token", before[2],
                      "real Chromium non-text metadata was read, not assumed")
        source = self.browser_state()[:7]
        attempts = self.word_edit_attempts()
        self.send_chord((gtk.KEY_LEFTSHIFT,))
        self.pump_until(lambda: self.word_edit_attempts() > attempts,
                        "unrepresentable selection refusal was attempted")
        self.wait_for_fresh_dom(self.browser_state()[-1], "fresh rich-refusal DOM")
        self.assertEqual(self.browser_state()[:7], source)
        self.assertEqual(self.rich_clipboard_snapshot(), before)
        self.assertIn("rejection_stage=clipboard_snapshot_targets", self.full_diagnostic())
        self.assertEqual(self.dispatches(), 0)

    def assert_oversized_native_word_refuses_and_recovers(self, source):
        self.xdo("type", "--clearmodifiers", "--delay", "0", source)
        self.pump_until(lambda: self.browser_state()[:3] ==
                        [source, len(source), len(source)], "oversized native source")
        attempts = self.word_edit_attempts()
        self.send_chord((gtk.KEY_LEFTCTRL,))
        self.pump_until(lambda: self.word_edit_attempts() > attempts,
                        "over-limit native word was attempted")
        self.wait_for_fresh_dom(self.browser_state()[-1], "fresh oversized refusal DOM")
        self.assertEqual(self.browser_state()[:3], [source, len(source), len(source)])
        self.assertEqual(self.dispatches(), 0)
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[:3] ==
                        [source + "f", len(source) + 1, len(source) + 1],
                        "next key appends instead of deleting oversized source")
        self.harness.send_key(gtk.KEY_SPACE)
        self.harness.type_word("ghbdtn")
        self.harness.send_key(gtk.KEY_PAUSE)
        expected = source + "f привет"
        self.pump_until(lambda: self.browser_state()[:3] ==
                        [expected, len(expected), len(expected)],
                        "fresh short word recovers after oversized refusal")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        for key in (gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT):
            self.assertFalse(self.key_is_down(key))

    def test_extensionless_oversized_word_refuses_and_recovers(self):
        self.assert_oversized_native_word_refuses_and_recovers("a" * 129)

    def test_extensionless_delimiter_extension_obeys_word_limit(self):
        self.assert_oversized_native_word_refuses_and_recovers("a" * 128 + ".")

    def test_extensionless_maximum_word_remains_eligible(self):
        self.assert_native_current_word_case("a" * 128, "A" * 128)

    def test_extensionless_selection_case_long_replay(self):
        source, expected = "a" * 128, "A" * 128
        self.xdo("type", "--clearmodifiers", "--delay", "0", source)
        self.pump_until(lambda: self.browser_state()[:3] == [source, 128, 128],
                        "pre-existing long application text")
        self.xdo("key", "ctrl+a")
        self.pump_until(lambda: self.browser_state()[:3] == [source, 0, 128],
                        "exact long user selection")
        started = time.monotonic()
        self.send_chord((gtk.KEY_LEFTALT,))
        self.pump_until(lambda: self.browser_state()[:3] == [expected, 128, 128],
                        "complete long selection case replay")
        self.pump_until(lambda: self.dispatches() == 1, "selection dispatched")
        self.assertLess(time.monotonic() - started, 4.0)
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[:3] ==
                        [expected + "f", 129, 129], "next unselected key")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.assertNotIn("[punto-e2e] clipboard_publish", self.full_diagnostic())
        self.assertEqual(self.output_count(gtk.KEY_PAUSE), 0)
        for key in (gtk.KEY_LEFTALT, gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT):
            self.assertFalse(self.key_is_down(key))

    def assert_delimiter_refusal_recovers(self, mode):
        self.xdo("key", "End")
        self.pump_until(lambda: self.browser_state()[:3] == ["hELLo.", 6, 6],
                        "actual source before native delimiter extension")
        attempts = self.word_edit_attempts()
        started = time.monotonic()
        self.send_chord((gtk.KEY_LEFTCTRL,))
        self.pump_until(lambda: self.word_edit_attempts() > attempts,
                        "native delimiter refusal completed")
        self.wait_for_fresh_dom(self.browser_state()[-1], "fresh native refusal DOM")
        self.assertEqual(self.browser_state()[-2], 2,
                         "negative control consumed the second actual native unit")
        expected_range = [0, 1] if mode == "foreign" else [6, 6]
        self.assertEqual(self.browser_state()[:3], ["hELLo.", *expected_range])
        self.assertEqual(self.dispatches(), 0)
        if mode == "foreign":
            self.assertIn("selection_confirmation_foreign", self.full_diagnostic())
            self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_PRIMARY), "h")
        if mode == "timeout":
            self.assertGreaterEqual(time.monotonic() - started, .3)
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.xdo("key", "ctrl+a")
        self.pump_until(lambda: self.browser_state()[:3] == ["hELLo.", 0, 6],
                        "fresh explicit user range after native refusal")
        self.send_chord((gtk.KEY_LEFTALT,))
        self.pump_until(lambda: self.browser_state()[:3] == ["HellO.", 6, 6],
                        "fresh selection succeeds without restarting Punto")
        self.pump_until(lambda: self.dispatches() == 1, "fresh recovery dispatched once")
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[:3] == ["HellO.f", 7, 7],
                        "next key follows the recovered caret")
        self.assertEqual(self.browser_state()[3:7], ["ghbdtn", 0, 0, "a"])
        self.assertEqual(self.output_count(gtk.KEY_PAUSE), 0)
        for key in (gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT, gtk.KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def test_extensionless_delimiter_unchanged_range_refuses_and_recovers(self):
        self.assert_delimiter_refusal_recovers("unchanged")

    def test_extensionless_delimiter_foreign_range_remains_selected(self):
        self.assert_delimiter_refusal_recovers("foreign")

    def test_extensionless_delimiter_late_selection_refuses_and_recovers(self):
        self.assert_delimiter_refusal_recovers("timeout")

    def test_extensionless_automatic_tab_orders_next_field(self):
        self.assert_automatic_tab_orders_next_field()

    def test_extensionless_redundant_status_preserves_automatic_tab(self):
        self.assert_automatic_tab_orders_next_field(redundant_status=True)

    def assert_automatic_tab_orders_next_field(self, *, redundant_status=False):
        self.assertEqual(gtk.ipc_request(b"SET_STATUS 1\n"), b"OK ENABLED\n")
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "source browser word before automatic Tab")
        if redundant_status:
            self.assertIn("origin_capture_result sequence=1 present=1",
                          self.full_diagnostic())
            state_path = pathlib.Path("/run/punto-control.state")
            before = dict(line.split("=", 1) for line in
                          state_path.read_text().splitlines())
            self.assertEqual(gtk.ipc_request(b"SET_STATUS 1\n"), b"OK ENABLED\n")
            after = dict(line.split("=", 1) for line in
                         state_path.read_text().splitlines())
            self.assertEqual(int(after["status_generation"]),
                             int(before["status_generation"]) + 1)
            self.assertEqual(after["config_generation"], before["config_generation"])
        sent_at = time.monotonic()
        self.harness.send_key(gtk.KEY_TAB)
        self.pump_until(lambda: self.output_count(gtk.KEY_TAB, 1) == 1,
                        "original browser Tab press")
        self.assertLess(time.monotonic() - sent_at, 1.0)
        self.pump_until(lambda: self.browser_state()[:7] ==
                        ["привет", 6, 6, "", 0, 0, "b"],
                        "source correction and empty next browser field")
        self.pump_until(lambda: self.output_count(gtk.KEY_TAB, 0) == 1,
                        "original browser Tab release")
        self.pump_until(lambda: self.dispatches() == 1, "automatic Tab dispatch")
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[3:7] == ["а", 1, 1, "b"],
                        "next key uses Russian in destination field")
        self.harness.send_key(gtk.KEY_BACKSPACE)
        self.harness.type_word("hello")
        self.pump_until(lambda: self.browser_state()[3:7] == ["руддщ", 5, 5, "b"],
                        "fresh word in destination field")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:7] ==
                        ["привет", 6, 6, "hello", 5, 5, "b"],
                        "fresh correction cannot reuse source-field history")
        self.pump_until(lambda: self.dispatches() == 2, "fresh field dispatch")
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after destination correction")
        self.assertEqual(self.browser_state()[9], [1, 1],
                         "no extra replay-generated browser Tab")
        self.assertEqual(self.output_count(gtk.KEY_TAB, 1), 1)
        self.assertEqual(self.output_count(gtk.KEY_TAB, 0), 1)
        self.assertEqual(self.output_count(gtk.KEY_PAUSE), 0)
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        for key in (gtk.KEY_TAB, gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT):
            self.assertFalse(self.key_is_down(key))

    def test_word_layout_selection_disturbance_never_inserts_at_caret(self):
        self.harness.type_word("ghbdtn")
        self.pump_until(
            lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
            "browser word before desktop selection disturbance",
        )
        before = self.browser_state()
        dispatches = self.dispatches()
        rejected = self.harness.diagnostic().count(
            "rejection_stage=layout_cleanup_foreign_raw_key"
        )
        self.layout_shortcut.selection_disturbance = True
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.layout_shortcut.selection_disturbances == 1,
            "browser word selection disturbed by desktop transition",
        )
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "rejection_stage=layout_cleanup_foreign_raw_key"
            ) > rejected,
            "browser word rejects disturbed visible selection",
        )
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after disturbed word rejection")
        after = self.browser_state()
        self.assertEqual(after[0], before[0])
        self.assertEqual(after[3:7], before[3:7])
        self.assertEqual(after[1:3], [6, 6])
        self.assertEqual(self.dispatches(), dispatches)

    def assert_four_manual_conversions(self):
        self.harness.type_word("ghbdtn")
        expected = (("привет", 1), ("ghbdtn", 0),
                    ("привет", 1), ("ghbdtn", 0))
        for dispatch, (text, group) in enumerate(expected, 1):
            with self.subTest(dispatch=dispatch):
                self.harness.send_key(gtk.KEY_PAUSE)
                self.pump_until(
                    lambda text=text: self.browser_state()[:3] == [text, 6, 6],
                    f"browser correction {dispatch}",
                )
                self.pump_until(lambda dispatch=dispatch:
                                self.dispatches() == dispatch,
                                f"dispatch receipt {dispatch}")
                self.assertEqual(self.keyboard_group(), group)
                desktop_group = (self.real_layout_bridge.role
                                 if self.real_layout_bridge is not None
                                 else self.desktop_source_role())
                self.assertEqual(desktop_group, group)

    def test_four_manual_conversions_retain_each_successful_receipt(self):
        daemon_pid = self.harness.process.pid
        daemon_start = gtk.process_start_time(daemon_pid)
        clipboard = self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD)
        clipboard_owner = self.clipboard_owner()
        self.assert_four_manual_conversions()
        self.send_chord((gtk.KEY_LEFTCTRL,), key=gtk.LETTER_CODES["a"])
        self.harness.send_key(gtk.KEY_BACKSPACE)
        self.pump_until(lambda: self.browser_state()[:3] == ["", 0, 0],
                        "physical clear after successful manual receipts")
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "fresh physical word after clear")
        right_releases = self.output_count(gtk.KEY_RIGHT, 0)
        self.harness.send_key(gtk.KEY_RIGHT)
        self.pump_until(lambda: self.output_count(gtk.KEY_RIGHT, 0) > right_releases,
                        "ordinary navigation delivered before history check")
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after navigation at end")
        self.assertEqual(self.browser_state()[:3], ["ghbdtn", 6, 6])
        self.assertEqual(self.stats_fields()[1]["history_words"], "0")
        self.assertEqual(self.dispatches(), 4)
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == ["привет", 6, 6],
                        "fresh manual preparation with cached PRIMARY")
        self.pump_until(lambda: self.dispatches() == 5, "fifth fresh dispatch")
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.desktop_source_role(), 1)
        self.harness.type_word("a")
        self.pump_until(lambda: self.browser_state()[:3] == ["приветф", 7, 7],
                        "next physical glyph follows actual RU source")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD), clipboard)
        self.assertEqual(self.clipboard_owner(), clipboard_owner)
        self.assertEqual(self.output_count(gtk.KEY_PAUSE), 0)
        for key in (gtk.KEY_PAUSE, gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT,
                    gtk.KEY_LEFTALT, gtk.KEY_RIGHT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(gtk.process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())

    def test_manual_conversion_uses_original_phase_read_deadline(self):
        daemon_pid = self.harness.process.pid
        self.assertEqual(self.selection_owner(), 0)
        marker = pathlib.Path("/run/punto-e2e-delay-primary-conversion-barrier")
        marker.touch(mode=0o600)
        self.assert_four_manual_conversions()
        self.assertFalse(marker.exists())
        for name in ("primary-conversion-submitted",
                     "primary-conversion-barrier-submitted",
                     "primary-conversion-barrier-delay-complete"):
            self.assertTrue(pathlib.Path(f"/run/punto-e2e-{name}").exists())
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after bounded read recovery")
        self.assertEqual(self.browser_state()[:3], ["ghbdtn", 6, 6])
        self.harness.type_word("k")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtnk", 7, 7],
                        "next key follows final EN desktop source")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.assertEqual(self.desktop_source_role(), 0)
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())
        self.assertEqual(self.output_count(gtk.KEY_PAUSE), 0)
        for key in (gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT, gtk.KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def test_hundred_manual_conversions_survive_failure_cycles(self):
        daemon_pid = self.harness.process.pid
        daemon_start = gtk.process_start_time(daemon_pid)
        clipboard = self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD)
        clipboard_owner = self.selection_owner("CLIPBOARD")
        marker = pathlib.Path("/run/punto-e2e-force-layout-bridge-reject")
        self.addCleanup(marker.unlink, missing_ok=True)
        rejection = (
            "Word edit dispatch status=0 "
            "rejection_stage=layout_bridge_test_rejected retry_policy=1")
        self.harness.type_word("ghbdtn")
        self.pump_until(
            lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
            "soak source word",
        )
        expected = "ghbdtn"
        for index in range(100):
            if index in (20, 50, 80):
                self.assertFalse(marker.exists(), "fault must be freshly armed")
                activations = self.layout_shortcut.activations
                group = self.keyboard_group()
                source = self.desktop_source_role()
                rejected = self.full_diagnostic().count(rejection)
                marker.touch(mode=0o600)
                self.assert_pause_rejected(
                    expected_stage="layout_bridge_test_rejected")
                self.assertFalse(marker.exists(), "typed rejection consumed")
                self.assertEqual(self.full_diagnostic().count(rejection),
                                 rejected + 1)
                self.assertEqual(self.browser_state()[:3], [expected, 6, 6])
                self.assertEqual(self.dispatches(), index)
                self.assertEqual(self.layout_shortcut.activations, activations)
                self.assertEqual(self.keyboard_group(), group)
                self.assertEqual(self.desktop_source_role(), source)
                self.assertEqual(self.selection_owner("CLIPBOARD"),
                                 clipboard_owner)
                self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                                 clipboard)
                for key in (gtk.KEY_PAUSE, gtk.KEY_LEFTCTRL,
                            gtk.KEY_LEFTSHIFT, gtk.KEY_LEFTALT, gtk.KEY_GRAVE):
                    self.assertFalse(self.key_is_down(key))
            self.harness.send_key(gtk.KEY_PAUSE)
            expected = "привет" if expected == "ghbdtn" else "ghbdtn"
            self.pump_until(
                lambda expected=expected: self.browser_state()[:3] ==
                [expected, 6, 6],
                f"soak correction {index + 1}",
            )
            self.pump_until(
                lambda index=index: self.dispatches() == index + 1,
                f"soak dispatch {index + 1}",
            )
            if index in (24, 49, 74):
                self.stats_fields()
                self.pump_for(0.02)
        self.assertEqual(self.keyboard_group(), 0)
        self.assertEqual(self.desktop_source_role(), 0)
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtnf", 7, 7],
                        "ordinary EN input after one hundred conversions")
        self.assertEqual(self.selection_owner("CLIPBOARD"),
                         clipboard_owner)
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD), clipboard)
        self.assertEqual(self.output_count(gtk.KEY_PAUSE), 0)
        for key in (gtk.KEY_PAUSE, gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT,
                    gtk.KEY_LEFTALT, gtk.KEY_GRAVE):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(gtk.process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())

    def test_new_words_context_refresh_and_idle_do_not_accumulate_state(self):
        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)
        self.assertEqual(gtk.ipc_request(b"SET_STATUS 1\n"), b"OK ENABLED\n")
        expected = ""
        cases = (("ghbdtn", "привет"), ("hello", "hello")) * 4
        for index, (physical, corrected) in enumerate(cases):
            self.harness.type_word(physical)
            self.harness.send_key(gtk.KEY_SPACE)
            expected += corrected + " "
            self.pump_until(
                lambda expected=expected: self.browser_state()[:3] ==
                [expected, len(expected), len(expected)],
                f"new automatic word {index + 1}",
            )
            if index == 3:
                self.publish_active_window(self.browser_window)
                self.stats_fields()
                self.pump_for(0.35)
        self.assertEqual(self.dispatches(), len(cases))
        self.harness.type_word("f")
        self.pump_until(
            lambda: self.browser_state()[:3] ==
            [expected + "f", len(expected) + 1, len(expected) + 1],
            "next physical character after varied-word idle",
        )
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(process_start_time(daemon_pid), daemon_start)

    def assert_post_dispatch_deadline_preserves_new_receipt(self, marker_name):
        self.first_manual_conversion()
        marker = pathlib.Path("/run", marker_name)
        marker.unlink(missing_ok=True)
        marker.touch(mode=0o600)
        self.addCleanup(marker.unlink, missing_ok=True)
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        f"second correction before {marker_name}")
        self.pump_until(lambda: self.dispatches() == 2,
                        f"second dispatch before {marker_name}")
        self.assertFalse(marker.exists(), f"{marker_name} was not reached")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == ["привет", 6, 6],
                        f"third correction after {marker_name}")
        self.pump_until(lambda: self.dispatches() == 3,
                        f"third dispatch after {marker_name}")

    def test_post_dispatch_deadline_before_context_preserves_new_receipt(self):
        self.assert_post_dispatch_deadline_preserves_new_receipt(
            "punto-e2e-expire-before-post-dispatch-context")

    def test_post_dispatch_deadline_in_wait_preserves_new_receipt(self):
        self.assert_post_dispatch_deadline_preserves_new_receipt(
            "punto-e2e-expire-in-post-dispatch-wait")

    def assert_post_dispatch_context_change_invalidates_new_receipt(
            self, *, after_retained, change, restore):
        authority = (self.harness.process.pid,
                     gtk.process_start_time(self.harness.process.pid),
                     self.clipboard_owner(),
                     self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD))
        if after_retained:
            self.first_manual_conversion()
        else:
            self.harness.type_word("ghbdtn")
            self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                            "browser source before first dispatch")
        marker_names = (
            "punto-e2e-arm-after-word-dispatch",
            "punto-e2e-after-word-dispatch",
            "punto-e2e-release-after-word-dispatch",
        )
        markers = {name: pathlib.Path("/run", name) for name in marker_names}
        for marker in markers.values():
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        self.addCleanup(markers["punto-e2e-release-after-word-dispatch"].touch,
                        exist_ok=True)
        markers["punto-e2e-arm-after-word-dispatch"].touch(mode=0o600)
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(markers["punto-e2e-after-word-dispatch"].exists,
                        "successful replay reached post-dispatch settlement")
        change()
        markers["punto-e2e-release-after-word-dispatch"].touch(mode=0o600)
        dispatch = 2 if after_retained else 1
        text = "ghbdtn" if after_retained else "привет"
        self.pump_until(lambda: self.dispatches() == dispatch,
                        "successful dispatch completes after context change")
        self.pump_until(lambda: self.browser_state()[:3] == [text, 6, 6],
                        "successful replay is visible after context change")
        restore()
        self.assert_pause_rejected()
        self.assert_pause_rejected()
        self.assert_fresh_physical_manual_recovery(authority)

    def post_dispatch_pointer_change(self, after_retained):
        original = self.pointer_position()
        self.assert_post_dispatch_context_change_invalidates_new_receipt(
            after_retained=after_retained,
            change=lambda: self.xdo("mousemove", str(original[0] + 40),
                                    str(original[1])),
            restore=lambda: self.xdo("mousemove", str(original[0]),
                                     str(original[1])),
        )

    def post_dispatch_focus_change(self, after_retained):
        gtk_window = self.window.get_window().get_xid()
        self.assert_post_dispatch_context_change_invalidates_new_receipt(
            after_retained=after_retained,
            change=lambda: self.xdo("windowfocus", "--sync", str(gtk_window)),
            restore=lambda: (self.xdo("windowfocus", "--sync",
                                      str(self.browser_window)),
                             self.publish_active_window(self.browser_window)),
        )

    def post_dispatch_lock_change(self, after_retained):
        self.assert_post_dispatch_context_change_invalidates_new_receipt(
            after_retained=after_retained,
            change=lambda: self.keyboard_locks(2),
            restore=lambda: self.keyboard_locks(0),
        )

    def test_post_dispatch_pointer_change_invalidates_first_receipt(self):
        self.post_dispatch_pointer_change(False)

    def test_post_dispatch_pointer_change_invalidates_retained_receipt(self):
        self.post_dispatch_pointer_change(True)

    def test_post_dispatch_focus_change_invalidates_first_receipt(self):
        self.post_dispatch_focus_change(False)

    def test_post_dispatch_focus_change_invalidates_retained_receipt(self):
        self.post_dispatch_focus_change(True)

    def test_post_dispatch_lock_change_invalidates_first_receipt(self):
        self.post_dispatch_lock_change(False)

    def test_post_dispatch_lock_change_invalidates_retained_receipt(self):
        self.post_dispatch_lock_change(True)

    def test_correction_uses_desktop_layout_shortcut(self):
        self.first_manual_conversion()
        self.pump_until(lambda: self.layout_shortcut.activations == 1,
                        "configured desktop layout shortcut")
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertFalse(self.key_is_down(gtk.KEY_LEFTCTRL))
        self.assertFalse(self.key_is_down(gtk.KEY_GRAVE))

    def test_correction_uses_reloaded_layout_shortcut(self):
        self.reload_layout_shortcut(
            gtk.KEY_LEFTALT, gtk.KEY_BACKSLASH, 8,
            "leftctrl", "leftalt", "grave", "backslash",
        )

        self.first_manual_conversion()
        self.assertEqual(self.layout_shortcut.activations, 1)
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertFalse(self.key_is_down(gtk.KEY_LEFTALT))
        self.assertFalse(self.key_is_down(gtk.KEY_BACKSLASH))

    def reload_layout_shortcut(self, modifier, key, modifier_mask,
                               old_modifier, new_modifier, old_key, new_key):
        self.layout_shortcut.stop()
        self.layout_shortcut = gtk.DesktopLayoutShortcut(
            self.x11.display,
            modifier=modifier,
            key=key,
            modifier_mask=modifier_mask,
        )
        self.layout_shortcut.start()
        self.addCleanup(self.layout_shortcut.stop)

        config = pathlib.Path("/tmp/punto-home/.config/punto/config.yaml")
        original = config.read_text(encoding="utf-8")
        self.addCleanup(config.write_text, original, encoding="utf-8")
        config.write_text(
            original.replace(f"modifier: {old_modifier}",
                             f"modifier: {new_modifier}")
                    .replace(f"key: {old_key}", f"key: {new_key}"),
            encoding="utf-8",
        )
        generation = int(self.stats_fields()[1]["config_generation"])
        self.assertEqual(gtk.ipc_request(b"RELOAD\n"), b"OK Scheduled\n")
        self.pump_until(
            lambda: int(self.stats_fields()[1]["config_generation"]) > generation
            and self.stats_fields()[1]["config_result"] == "ok",
            "custom layout shortcut config commit",
        )

    def test_stale_primary_reset_uses_reloaded_layout_shortcut(self):
        self.reload_layout_shortcut(
            gtk.KEY_LEFTALT, gtk.KEY_BACKSLASH, 8,
            "leftctrl", "leftalt", "grave", "backslash",
        )
        self.assert_layout_ack_failure_recovers()
        self.assertEqual(self.layout_shortcut.activations, 3)
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertFalse(self.key_is_down(gtk.KEY_LEFTALT))
        self.assertFalse(self.key_is_down(gtk.KEY_BACKSLASH))

    def test_correction_uses_modifier_only_layout_shortcut(self):
        self.layout_shortcut.stop()
        self.layout_shortcut = gtk.DesktopLayoutShortcut(
            self.x11.display,
            modifier=gtk.KEY_LEFTCTRL,
            key=gtk.KEY_RIGHTCTRL,
            modifier_mask=4,
        )
        self.layout_shortcut.start()
        self.addCleanup(self.layout_shortcut.stop)

        config = pathlib.Path("/tmp/punto-home/.config/punto/config.yaml")
        original = config.read_text(encoding="utf-8")
        self.addCleanup(config.write_text, original, encoding="utf-8")
        config.write_text(
            original.replace("key: grave", "key: rightctrl"),
            encoding="utf-8",
        )
        generation = int(self.stats_fields()[1]["config_generation"])
        self.assertEqual(gtk.ipc_request(b"RELOAD\n"), b"OK Scheduled\n")
        self.pump_until(
            lambda: int(self.stats_fields()[1]["config_generation"]) > generation
            and self.stats_fields()[1]["config_result"] == "ok",
            "modifier-only layout shortcut config commit",
        )

        self.first_manual_conversion()
        self.assertEqual(self.layout_shortcut.activations, 1)
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertFalse(self.key_is_down(gtk.KEY_LEFTCTRL))
        self.assertFalse(self.key_is_down(gtk.KEY_RIGHTCTRL))
        activations = self.layout_shortcut.activations
        self.send_chord((gtk.KEY_LEFTCTRL,), key=gtk.KEY_RIGHTCTRL)
        self.pump_until(
            lambda: self.layout_shortcut.activations == activations + 1
            and self.keyboard_group() == 0,
            "physical modifier-only desktop shortcut",
        )
        self.harness.send_key(gtk.KEY_SPACE)
        self.harness.type_word("ghbdtn")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.browser_state()[:3] == ["привет привет", 13, 13],
            "correction after physical modifier-only shortcut",
        )
        self.assertEqual(self.dispatches(), 2)

    def test_layout_shortcut_with_num_lock(self):
        self.keyboard_locks(16)
        self.first_manual_conversion()
        self.pump_until(lambda: self.layout_shortcut.activations == 1,
                        "desktop shortcut with lock modifiers")
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertEqual(self.keyboard_locks(), 16)

    def test_physical_layout_shortcut_lock_variants(self):
        for locks in (0, 2, 16, 18):
            with self.subTest(locks=locks):
                self.set_desktop_layout(0)
                self.keyboard_locks(locks)
                activations = self.layout_shortcut.activations
                self.send_chord((gtk.KEY_LEFTCTRL,), key=gtk.KEY_GRAVE)
                self.pump_until(
                    lambda: self.layout_shortcut.activations == activations + 1
                    and self.keyboard_group() == 1,
                    f"desktop shortcut with lock mask {locks}",
                )
                self.assertEqual(self.desktop_source_role(), 1)
                self.assertEqual(self.keyboard_locks(), locks)

    def test_delayed_layout_shortcut_uses_shared_macro_budget(self):
        self.layout_shortcut.handling_delay = 0.05
        self.first_manual_conversion()
        self.assertEqual(self.layout_shortcut.activations, 1)
        self.assertEqual(self.keyboard_group(), 1)

    def test_slow_desktop_layout_transition_still_corrects_word(self):
        self.layout_shortcut.handling_delay = 0.45
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "browser typed source")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == ["привет", 6, 6],
                        "word corrected after slow desktop layout transition")
        self.assertEqual(self.dispatches(), 1)
        self.assertEqual(self.keyboard_group(), 1)
        self.assertFalse(self.key_is_down(gtk.KEY_LEFTCTRL))
        self.assertFalse(self.key_is_down(gtk.KEY_GRAVE))

    def test_transient_modifier_after_bridge_reply_fails_closed(self):
        marker = pathlib.Path(
            "/run/punto-e2e-inject-own-layout-modifier-state")
        self.addCleanup(marker.unlink, missing_ok=True)
        marker.touch(mode=0o600)
        self.layout_shortcut.handling_delay = 0.05
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "browser typed before transient layout state")
        rejected = self.harness.diagnostic().count(
            "rejection_stage=layout_bridge_xkb_state")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "rejection_stage=layout_bridge_xkb_state") > rejected,
            "transient modifier rejection after bridge reply")
        self.assertFalse(marker.exists())
        self.assertEqual(self.browser_state()[:3], ["ghbdtn", 6, 6])
        self.assertEqual(self.dispatches(), 0)
        self.assertIn("rejection_stage=layout_bridge_xkb_state",
                      self.harness.diagnostic())

    def test_focus_transition_after_bridge_reply_fails_closed(self):
        daemon_pid = self.harness.process.pid
        daemon_start = gtk.process_start_time(daemon_pid)
        gtk_window = self.window.get_window().get_xid()
        self.layout_shortcut.focus_transition = (gtk_window,
                                                 self.browser_window)
        self.layout_shortcut.handling_delay = 0.05
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "browser typed before desktop focus transition")
        original = self.browser_state()
        clipboard = self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD)
        rejected = self.harness.diagnostic().count(
            "rejection_stage=layout_bridge_context_after")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "rejection_stage=layout_bridge_context_after") > rejected,
            "focus transition rejection after bridge reply")
        self.assertEqual(self.layout_shortcut.focus_transitions, 1)
        self.wait_for_fresh_dom(self.browser_state()[-1],
                                "fresh browser range after focus-ABA refusal")
        self.assertRegex(
            self.harness.diagnostic(),
            r"selection admission stage=\S+ context_result=4 [^\n]*"
            r"focus_result=0 focus_reply=(?:[2-9]|[1-9][0-9]+)(?:\n|$)",
            "actual focus drift must retain its successful reply identity",
        )
        self.assertEqual(self.browser_state()[:3], ["ghbdtn", 0, 6])
        self.assertEqual(self.browser_state()[3:7], original[3:7])
        self.assertEqual(self.dispatches(), 0)
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD), clipboard)
        for key in (gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT, gtk.KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))
        self.assertIn("rejection_stage=layout_bridge_context_after",
                      self.harness.diagnostic())
        self.pump_until(
            lambda: self.layout_shortcut.activations == 1 and
            self.keyboard_group() == 1,
            "late target layout after transient focus transition",
        )
        self.layout_shortcut.focus_transition = None
        self.layout_shortcut.handling_delay = 0
        self.harness.send_key(gtk.KEY_END)
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "new physical navigation resolves prepared range")
        self.harness.send_key(gtk.KEY_SPACE)
        self.harness.type_word("hello")
        self.pump_until(
            lambda: self.browser_state()[:3] == ["ghbdtn руддщ", 12, 12],
            "fresh word follows the observed late target layout",
        )
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.browser_state()[:3] == ["ghbdtn hello", 12, 12],
            "focus-transition rejection recovers without a manual toggle",
        )
        self.assertEqual(self.dispatches(), 1)
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn hellof", 13, 13],
                        "next physical key after fresh focus recovery")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD), clipboard)
        for key in (gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT, gtk.KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(gtk.process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())

    def test_unobserved_focus_return_before_cleanup_expiry_revokes_lineage(self):
        markers = {
            name: pathlib.Path("/run", "punto-e2e-" + name)
            for name in (
                "exhaust-layout-cleanup-budget",
                "layout-cleanup-budget-exhausting",
                "layout-cleanup-budget-exhausted",
                "arm-cleanup-receipt-retained",
                "cleanup-receipt-retained",
            )
        }
        for marker in markers.values():
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        markers["exhaust-layout-cleanup-budget"].touch(mode=0o600)
        markers["arm-cleanup-receipt-retained"].touch(mode=0o600)
        daemon_pid = self.harness.process.pid
        clipboard_before = self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD)
        self.layout_shortcut.focus_transition = (
            self.window.get_window().get_xid(), self.browser_window)
        self.layout_shortcut.handling_delay = 0.05
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "source before unconfirmed focus return")
        attempts = self.word_edit_attempts()
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(markers["layout-cleanup-budget-exhausting"].exists,
                        "budget exhaustion after observed bridge focus change")
        self.assertFalse(markers["exhaust-layout-cleanup-budget"].exists())
        self.assertEqual(self.layout_shortcut.focus_transitions, 1)
        self.pump_until(
            lambda: self.layout_shortcut.activations == 1 and
            self.keyboard_group() == 1,
            "desktop focus and layout returned during blocked cleanup",
        )
        self.pump_until(markers["layout-cleanup-budget-exhausted"].exists,
                        "cleanup hard deadline exhausted")
        self.pump_until(lambda: self.word_edit_attempts() > attempts,
                        "expired operation returned without another focus read")
        self.assertIn("rejection_stage=layout_bridge_context_after",
                      self.harness.diagnostic())
        self.assertTrue(markers["arm-cleanup-receipt-retained"].exists(),
                        "unobserved focus return must not authorize retention")
        self.assertFalse(markers["cleanup-receipt-retained"].exists())
        self.assertEqual(self.dispatches(), 0)
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after expired cleanup")
        self.assertEqual(self.browser_state()[:3], ["ghbdtn", 0, 6])
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         clipboard_before)
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())
        for key in (gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT, gtk.KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def test_post_bridge_xkb_observation_timeout_refuses_then_recovers(self):
        arm = pathlib.Path("/run/punto-e2e-withhold-post-bridge-xkb-reply")
        consumed = pathlib.Path("/run/punto-e2e-post-bridge-xkb-reply-withheld")
        for marker in (arm, consumed):
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        arm.touch(mode=0o600)
        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)
        clipboard_before = self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "source before a missing post-bridge XKB reply")
        attempts = self.word_edit_attempts()
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(consumed.exists, "actual post-bridge XKB reply withheld")
        self.assertFalse(arm.exists())
        self.pump_until(lambda: self.word_edit_attempts() > attempts,
                        "typed observation timeout completed")
        self.assertIn("rejection_stage=layout_bridge_xkb_observation_timeout",
                      self.harness.diagnostic())
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh unchanged DOM after missing reply")
        self.assertEqual(self.browser_state()[:3], ["ghbdtn", 6, 6])
        self.assertEqual(self.dispatches(), 0)
        self.assertEqual(self.keyboard_group(), 1)
        self.harness.send_key(gtk.KEY_SPACE)
        self.harness.type_word("hello")
        self.pump_until(
            lambda: self.browser_state()[:3] == ["ghbdtn руддщ", 12, 12],
            "fresh word follows the actual late RU source",
        )
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn hello", 12, 12],
                        "fresh correction recovers on the same live connection")
        self.assertEqual(self.dispatches(), 1)
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn hellof", 13, 13],
                        "next ordinary key uses the recovered EN source")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         clipboard_before)
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())
        for key in (gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT, gtk.KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def test_transient_layout_modifier_rejects_before_text_mutation(self):
        markers = {
            name: pathlib.Path("/run", "punto-e2e-" + name)
            for name in ("arm-layout-settle-window",
                         "layout-settle-too-early",
                         "layout-settle-observed")
        }
        for marker in markers.values():
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        transient = pathlib.Path(
            "/run/punto-e2e-inject-own-layout-modifier-state")
        transient.unlink(missing_ok=True)
        transient.touch(mode=0o600)
        self.addCleanup(transient.unlink, missing_ok=True)
        markers["arm-layout-settle-window"].touch(mode=0o600)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "browser source before transient modifier")
        rejected = self.harness.diagnostic().count(
            "rejection_stage=layout_bridge_xkb_state")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "rejection_stage=layout_bridge_xkb_state") > rejected,
            "transient layout modifier rejected",
        )
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "transient modifier preserved source text")
        self.assertFalse(markers["arm-layout-settle-window"].exists())
        self.assertFalse(markers["layout-settle-too-early"].exists())
        self.assertFalse(markers["layout-settle-observed"].exists())
        self.assertEqual(self.dispatches(), 0)

    def test_foreign_modifier_during_layout_transition_rejects_correction(self):
        marker = pathlib.Path(
            "/run/punto-e2e-inject-foreign-layout-modifier-state")
        self.addCleanup(marker.unlink, missing_ok=True)
        receipt_arm = pathlib.Path("/run/punto-e2e-arm-cleanup-receipt-retained")
        receipt_retained = pathlib.Path("/run/punto-e2e-cleanup-receipt-retained")
        for receipt_marker in (receipt_arm, receipt_retained):
            receipt_marker.unlink(missing_ok=True)
            self.addCleanup(receipt_marker.unlink, missing_ok=True)
        receipt_arm.touch(mode=0o600)
        marker.touch(mode=0o600)
        self.layout_shortcut.handling_delay = 0.05
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "browser typed before foreign modifier state")
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            "foreign modifier layout rejection",
        )
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after foreign modifier rejection")
        self.assertFalse(marker.exists())
        self.assertTrue(receipt_arm.exists(), "hard revocation must not retain authority")
        self.assertFalse(receipt_retained.exists())
        self.assertEqual(self.browser_state()[:3], ["ghbdtn", 6, 6])
        self.assertEqual(self.dispatches(), 0)

    def test_layout_transition_past_extended_budget_does_not_mutate_text(self):
        daemon_pid = self.harness.process.pid
        self.layout_shortcut.handling_delay = 3.6
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "browser typed source")
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            "desktop layout transition budget rejection",
            timeout=5,
        )
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after layout timeout")
        self.assertEqual(self.browser_state()[:3], ["ghbdtn", 6, 6])
        self.assertEqual(self.dispatches(), 0)
        self.pump_until(lambda: self.layout_shortcut.activations == 1,
                        "late desktop layout activation")
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after late activation")
        self.pump_until(
            lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
            "late activation cleanup collapses only Punto selection",
        )
        self.assertFalse(self.key_is_down(gtk.KEY_LEFTCTRL))
        self.assertFalse(self.key_is_down(gtk.KEY_GRAVE))
        self.layout_shortcut.handling_delay = 0
        self.send_chord((gtk.KEY_LEFTCTRL,), key=gtk.KEY_GRAVE)
        self.pump_until(
            lambda: self.layout_shortcut.activations == 2
            and self.keyboard_group() == 0,
            "physical reset after late desktop activation",
        )
        self.harness.send_key(gtk.KEY_SPACE)
        self.harness.type_word("ghbdtn")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.browser_state()[:3] == ["ghbdtn привет", 13, 13],
            "fresh correction after late layout timeout",
        )
        self.assertEqual(self.layout_shortcut.activations, 3)
        self.assertEqual(self.dispatches(), 1)
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())

    def assert_layout_cleanup_phase_recovers_without_manual_toggle(
            self, phase, *, cleanup_delay=False, cleanup_focus_retry=False,
            deadline_boundary=False):
        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)
        arm = pathlib.Path(f"/run/punto-e2e-arm-layout-cleanup-phase-{phase}")
        reached = pathlib.Path(
            f"/run/punto-e2e-layout-cleanup-phase-{phase}-reached")
        release = pathlib.Path(
            f"/run/punto-e2e-release-layout-cleanup-phase-{phase}")
        receipt_arm = pathlib.Path("/run/punto-e2e-arm-cleanup-receipt-retained")
        receipt_retained = pathlib.Path("/run/punto-e2e-cleanup-receipt-retained")
        receipt_markers = (receipt_arm, receipt_retained) if phase == 3 else ()
        delay = pathlib.Path("/run/punto-e2e-delay-layout-cleanup-150ms")
        delay_reached = pathlib.Path(
            "/run/punto-e2e-layout-cleanup-delay-150ms-reached"
        )
        delay_markers = (delay, delay_reached) if cleanup_delay else ()
        focus_fault = pathlib.Path(
            "/run/punto-e2e-change-desktop-cleanup-focus-once"
        )
        focus_armed = pathlib.Path(
            "/run/punto-e2e-desktop-cleanup-focus-change-armed"
        )
        focus_markers = (focus_fault, focus_armed) if cleanup_focus_retry else ()
        boundary_fault = pathlib.Path(
            "/run/punto-e2e-expire-before-layout-context"
        )
        boundary_consumed = pathlib.Path(
            "/run/punto-e2e-expire-before-layout-context-consumed"
        )
        boundary_markers = ((boundary_fault, boundary_consumed)
                            if deadline_boundary else ())
        for marker in (arm, reached, release, *receipt_markers, *delay_markers,
                       *focus_markers, *boundary_markers):
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        arm.touch(mode=0o600)
        if deadline_boundary:
            self.assertEqual(phase, 2, "deadline fault belongs only to phase 2")
            boundary_fault.touch(mode=0o600)
        if cleanup_delay:
            delay.touch(mode=0o600)
        if cleanup_focus_retry:
            focus_fault.touch(mode=0o600)
        self.layout_shortcut.arm_blocked_activation()
        self.addCleanup(self.layout_shortcut.permit_activation.set)

        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "browser source before cleanup race")
        attempts = self.harness.diagnostic().count("Word edit dispatch status=")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(self.layout_shortcut.activation_started.is_set,
                        "desktop activation held past transition budget")
        self.pump_until(reached.exists, "word editor entered cleanup phase")
        if cleanup_delay:
            self.assertTrue(delay_reached.exists(),
                            "causal cleanup delay was consumed")
        if cleanup_focus_retry:
            self.assertTrue(focus_armed.exists(),
                            "causal cleanup focus change was consumed")
        if phase == 3:
            receipt_arm.touch(mode=0o600)
        self.layout_shortcut.permit_activation.set()
        self.pump_until(
            lambda: self.layout_shortcut.activations == 1 and
            self.keyboard_group() == 1,
            "desktop activation completed during cleanup",
        )
        release.touch(mode=0o600)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=") > attempts,
            "cleanup-race attempt completed",
        )
        if phase == 2:
            if deadline_boundary:
                self.assertTrue(boundary_consumed.exists(),
                                "exact post-bridge deadline fault was consumed")
                expired_context = re.findall(
                    r"selection admission stage=\S+ context_result=1 "
                    r"[^\n]*remaining_us=(-?\d+)",
                    self.harness.diagnostic(),
                )
                self.assertTrue(expired_context,
                                "post-bridge wait refusal was observed")
                self.assertLessEqual(int(expired_context[-1]), 0,
                                     "original layout deadline was exhausted")
            self.assertRegex(
                self.harness.diagnostic(),
                r"rejection_stage=layout_bridge_xkb_(?:observation_)?timeout ",
            )
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "cleanup race preserved source text and caret")
        self.assertEqual(self.dispatches(), 0)
        if phase == 3:
            self.assertTrue(receipt_retained.exists(),
                            "safe cleanup retained its exact receipt")

        self.harness.send_key(gtk.KEY_SPACE)
        self.harness.type_word("hello")
        self.pump_until(
            lambda: self.browser_state()[:3] == ["ghbdtn руддщ", 12, 12],
            "new physical word follows actual Russian layout",
        )
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.browser_state()[:3] == ["ghbdtn hello", 12, 12],
            "next word recovers without manual layout compensation",
        )
        self.assertEqual(self.layout_shortcut.activations, 2)
        self.assertEqual(self.dispatches(), 1)
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())

    def test_layout_activation_before_cleanup_recovers_without_manual_toggle(self):
        self.assert_layout_cleanup_phase_recovers_without_manual_toggle(0)

    def test_layout_activation_during_cleanup_recovers_without_manual_toggle(self):
        self.assert_layout_cleanup_phase_recovers_without_manual_toggle(1)

    def test_layout_activation_after_cleanup_recovers_without_manual_toggle(self):
        self.assert_layout_cleanup_phase_recovers_without_manual_toggle(
            2, deadline_boundary=True)

    def test_layout_activation_after_cleanup_right_recovers_without_manual_toggle(self):
        self.assert_layout_cleanup_phase_recovers_without_manual_toggle(3)

    def test_delayed_layout_cleanup_retains_recovery_budget(self):
        self.assert_layout_cleanup_phase_recovers_without_manual_toggle(
            3, cleanup_delay=True)

    def test_transient_cleanup_focus_observation_recovers(self):
        self.assert_layout_cleanup_phase_recovers_without_manual_toggle(
            3, cleanup_focus_retry=True)

    def test_selection_revalidation_failure_collapses_and_recovers(self):
        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)
        clipboard_before = self.plain_clipboard_snapshot()
        markers = tuple(
            pathlib.Path("/run", name)
            for name in (
                "punto-e2e-arm-fail-selection-revalidation",
                "punto-e2e-selection-revalidation-fault-armed",
                "punto-e2e-primary-payload-read-failed",
                "punto-e2e-arm-layout-cleanup-phase-3",
                "punto-e2e-layout-cleanup-phase-3-reached",
                "punto-e2e-release-layout-cleanup-phase-3",
            )
        )
        for marker in markers:
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        markers[0].touch(mode=0o600)
        markers[3].touch(mode=0o600)
        self.addCleanup(markers[5].touch, exist_ok=True)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "source before selection revalidation failure")
        self.assertEqual(self.browser_state()[-2], [0, 0])
        attempts = self.word_edit_attempts()
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(markers[1].exists,
                        "selection revalidation fault armed")
        self.pump_until(markers[2].exists,
                        "selection revalidation read failed")
        self.pump_until(markers[4].exists,
                        "revalidation cleanup completed Right lifecycle")
        markers[5].touch(mode=0o600)
        self.pump_until(lambda: self.word_edit_attempts() > attempts,
                        "selection revalidation attempt completed")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "failed revalidation collapsed Punto selection")
        self.pump_until(lambda: self.browser_state()[-2] == [1, 1],
                        "one complete browser cleanup Right pair")
        self.assertEqual(self.dispatches(), 0)
        self.pump_until(lambda: self.keyboard_group() == 1,
                        "target layout remains observed after revalidation")
        self.harness.send_key(gtk.KEY_SPACE)
        self.harness.type_word("hello")
        self.pump_until(
            lambda: self.browser_state()[:3] == ["ghbdtn руддщ", 12, 12],
            "fresh word follows target layout after revalidation failure",
        )
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.browser_state()[:3] == ["ghbdtn hello", 12, 12],
            "next correction recovers after revalidation failure",
        )
        self.assertEqual(self.dispatches(), 1)
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn hellof", 13, 13],
                        "ordinary next EN key after genuine recovery")
        self.assertEqual(self.browser_state()[-2], [1, 1])
        self.assertEqual(self.plain_clipboard_snapshot(), clipboard_before)
        for key in (gtk.KEY_PAUSE, gtk.KEY_RIGHT, gtk.KEY_LEFTCTRL,
                    gtk.KEY_LEFTSHIFT, gtk.KEY_GRAVE):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())

    def test_selection_confirmation_has_its_own_phase_budget(self):
        delay = pathlib.Path(
            "/run/punto-e2e-age-selection-confirmation-budget"
        )
        reached = pathlib.Path(
            "/run/punto-e2e-selection-confirmation-budget-aged"
        )
        for marker in (delay, reached):
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        delay.touch(mode=0o600)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "source before delayed selection confirmation")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(reached.exists, "selection confirmation budget aged")
        self.pump_until(lambda: self.browser_state()[:3] == ["привет", 6, 6],
                        "correction after delayed selection confirmation")
        self.assertEqual(self.dispatches(), 1)

    def test_selection_confirmation_timeout_collapses_and_recovers(self):
        expire = pathlib.Path(
            "/run/punto-e2e-expire-selection-confirmation-after-observation"
        )
        reached = pathlib.Path(
            "/run/punto-e2e-selection-confirmation-expired"
        )
        for marker in (expire, reached):
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        expire.touch(mode=0o600)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "source before confirmation timeout")
        attempts = self.word_edit_attempts()
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(reached.exists, "selection confirmation expired")
        self.pump_until(lambda: self.word_edit_attempts() > attempts,
                        "timed-out confirmation attempt completed")
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after confirmation cleanup")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "timed-out confirmation collapsed selection")
        self.assertEqual(self.dispatches(), 0)
        self.harness.send_key(gtk.KEY_SPACE)
        self.harness.type_word("hello")
        self.pump_until(
            lambda: self.browser_state()[:3] == ["ghbdtn hello", 12, 12],
            "fresh input appends after confirmation timeout",
        )
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.browser_state()[:3] == ["ghbdtn руддщ", 12, 12],
                        "correction recovered after confirmation timeout")
        self.assertEqual(self.dispatches(), 1)

    def assert_post_cleanup_right_change_rejects_receipt(
            self, change, restore, *, expected_stage=None,
            phase3_injection=None, expected_authority_reason=None,
            typed_rejection=False):
        daemon_pid = self.harness.process.pid
        markers = {
            name: pathlib.Path("/run", name)
            for name in (
                "punto-e2e-arm-layout-cleanup-phase-3",
                "punto-e2e-layout-cleanup-phase-3-reached",
                "punto-e2e-release-layout-cleanup-phase-3",
            )
        }
        for marker in markers.values():
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        delivered = None
        if phase3_injection is not None:
            injection = pathlib.Path(
                "/run", f"punto-e2e-inject-layout-cleanup-{phase3_injection}"
            )
            delivered = pathlib.Path(str(injection) + "-delivered")
            for marker in (injection, delivered):
                marker.unlink(missing_ok=True)
                self.addCleanup(marker.unlink, missing_ok=True)
            injection.touch(mode=0o600)
        receipt_arm = pathlib.Path("/run/punto-e2e-arm-cleanup-receipt-retained")
        receipt_retained = pathlib.Path("/run/punto-e2e-cleanup-receipt-retained")
        for marker in (receipt_arm, receipt_retained):
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        authority_markers = {
            reason: pathlib.Path(
                "/run", f"punto-e2e-cleanup-authority-{reason}"
            )
            for reason in ("selection-identity", "session", "timeout")
        }
        for marker in authority_markers.values():
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        self.addCleanup(
            markers["punto-e2e-release-layout-cleanup-phase-3"].touch,
            exist_ok=True,
        )
        markers["punto-e2e-arm-layout-cleanup-phase-3"].touch(mode=0o600)
        if typed_rejection:
            initial_fault = pathlib.Path("/run/punto-e2e-force-layout-bridge-reject")
            initial_fault.unlink(missing_ok=True)
            self.addCleanup(initial_fault.unlink, missing_ok=True)
            initial_fault.touch(mode=0o600)
            activations = self.layout_shortcut.activations
        else:
            self.layout_shortcut.arm_blocked_activation()
            self.addCleanup(self.layout_shortcut.permit_activation.set)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "source before post-Right control")
        clipboard_before = self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD)
        attempts = self.word_edit_attempts()
        if typed_rejection:
            right_before_cleanup = self.browser_state()[-2]
        self.harness.send_key(gtk.KEY_PAUSE)
        if not typed_rejection:
            self.pump_until(self.layout_shortcut.activation_started.is_set,
                            "desktop activation held for post-Right control")
            keys_before_cleanup = self.browser_state()[7]
        self.pump_until(
            markers["punto-e2e-layout-cleanup-phase-3-reached"].exists,
            "cleanup completed its own Right lifecycle",
            timeout=6,
        )
        if typed_rejection:
            self.assertFalse(initial_fault.exists(), "typed rejection consumed")
            self.pump_until(
                lambda: self.browser_state()[-2] == [
                    right_before_cleanup[0] + 1, right_before_cleanup[1] + 1],
                "exact cleanup Right lifecycle before user selection changes",
            )
        if delivered is not None:
            self.assertTrue(delivered.exists(), "private X11 injection accepted")
        receipt_arm.touch(mode=0o600)
        stage_count = 0
        if expected_stage is not None:
            stage_count = self.full_diagnostic().count(
                f"rejection_stage={expected_stage}"
            )
        change()
        user_state = self.browser_state()
        markers["punto-e2e-release-layout-cleanup-phase-3"].touch(mode=0o600)
        self.pump_until(lambda: self.word_edit_attempts() > attempts,
                        "post-Right control completed")
        if typed_rejection:
            self.assertEqual(self.browser_state()[-2], [
                right_before_cleanup[0] + 1, right_before_cleanup[1] + 1])
            self.assertEqual(self.layout_shortcut.activations, activations)
            self.assertEqual(self.keyboard_group(), 0)
        else:
            expected_browser_keys = 2 if delivered is not None else 1
            self.pump_until(
                lambda: self.browser_state()[7]
                >= keys_before_cleanup + expected_browser_keys,
                "cleanup Right and any independent key reached Chromium",
            )
            self.layout_shortcut.permit_activation.set()
            self.pump_until(
                lambda: self.layout_shortcut.activations == 1 and
                self.keyboard_group() == 1,
                "late target layout during post-Right control",
            )
        if expected_stage is not None:
            self.assertGreater(
                self.full_diagnostic().count(f"rejection_stage={expected_stage}"),
                stage_count,
                self.full_diagnostic(),
            )
        if expected_authority_reason is not None:
            self.assertTrue(
                authority_markers[expected_authority_reason].exists(),
                f"target authority reason {expected_authority_reason} reached",
            )
            self.assertFalse(
                authority_markers["timeout"].exists(),
                "authority control was not masked by cleanup timeout",
            )
        self.assertFalse(receipt_retained.exists(),
                         "unsafe cleanup did not retain selection authority")
        self.assertTrue(receipt_arm.exists(),
                        "retention oracle remained armed")
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "post-Right rejection DOM state")
        after = self.browser_state()
        self.assertEqual(after[0], user_state[0])
        self.assertEqual(after[1:3], [6, 6])
        self.assertEqual(after[3:7], user_state[3:7])
        self.assertEqual(after[8], user_state[8])
        self.assertEqual(
            self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD), clipboard_before
        )
        self.assertEqual(self.dispatches(), 0)
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())
        restore()

    def test_post_cleanup_right_foreign_key_rejects_receipt(self):
        self.assert_post_cleanup_right_change_rejects_receipt(
            lambda: None, lambda: None,
            expected_stage="layout_cleanup_foreign_raw_key",
            phase3_injection="f8")

    def test_post_cleanup_right_extra_user_right_rejects_receipt(self):
        self.assert_post_cleanup_right_change_rejects_receipt(
            lambda: None, lambda: None,
            expected_stage="layout_cleanup_foreign_raw_key",
            phase3_injection="right")

    def test_post_cleanup_right_pointer_change_rejects_receipt(self):
        original = self.pointer_position()
        self.assert_post_cleanup_right_change_rejects_receipt(
            lambda: self.xdo("mousemove", str(original[0] + 40),
                            str(original[1])),
            lambda: self.xdo("mousemove", str(original[0]), str(original[1])),
        )

    def test_post_cleanup_right_lock_change_rejects_receipt(self):
        self.assert_post_cleanup_right_change_rejects_receipt(
            lambda: self.keyboard_locks(2), lambda: self.keyboard_locks(0))

    def test_post_cleanup_right_session_change_invalidates_receipt(self):
        switch = pathlib.Path("/run/punto-e2e-switch-session")
        committed = pathlib.Path("/run/punto-e2e-new-session-committed")
        revoke = pathlib.Path("/run/punto-e2e-revoke-cleanup-lease")
        commanded = pathlib.Path(
            "/run/punto-e2e-cleanup-lease-revocation-commanded"
        )
        revoked = pathlib.Path("/run/punto-e2e-cleanup-lease-revoked")
        for marker in (revoke, commanded, revoked):
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)

        def switch_session():
            switch.touch(mode=0o600)
            revoke.touch(mode=0o600)
            self.pump_until(commanded.exists,
                            "cleanup lease revocation command observed")

        self.assert_post_cleanup_right_change_rejects_receipt(
            switch_session, lambda: None,
            expected_stage="layout_cleanup_session",
            expected_authority_reason="session")
        self.assertTrue(revoked.exists(), "cleanup write lease was revoked")
        self.pump_until(committed.exists, "replacement session committed")

    def test_post_cleanup_right_focus_change_rejects_receipt(self):
        gtk_window = self.window.get_window().get_xid()
        self.assert_post_cleanup_right_change_rejects_receipt(
            lambda: self.xdo("windowfocus", "--sync", str(gtk_window)),
            lambda: (
                self.xdo("windowfocus", "--sync", str(self.browser_window)),
                self.publish_active_window(self.browser_window),
            ),
        )

    def test_post_cleanup_right_same_text_selection_rejects_receipt(self):
        original_pointer = self.pointer_position()

        def select_same_text_without_context_drift():
            self.select_field_b_word(delay=1)
            self.xdo("mousemove", str(original_pointer[0]),
                     str(original_pointer[1]))

        def restore_browser_field():
            left, top, decoration = self.browser_state()[8]
            self.xdo("mousemove", "--window", str(self.browser_window),
                     str(int(left + 30)), str(int(top + decoration - 40)))
            self.xdo("click", "1")

        self.assert_post_cleanup_right_change_rejects_receipt(
            select_same_text_without_context_drift, restore_browser_field,
            expected_stage="layout_cleanup_selection_identity",
            expected_authority_reason="selection-identity", typed_rejection=True)

    def test_same_text_control_fails_when_identity_check_is_bypassed(self):
        bypass = pathlib.Path(
            "/run/punto-e2e-bypass-cleanup-selection-identity"
        )
        bypass.unlink(missing_ok=True)
        bypass.touch(mode=0o600)
        self.addCleanup(bypass.unlink, missing_ok=True)
        original_pointer = self.pointer_position()
        clipboard = self.plain_clipboard_snapshot()
        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)

        def select_same_text_without_context_drift():
            self.select_field_b_word(delay=1)
            self.xdo("mousemove", str(original_pointer[0]),
                     str(original_pointer[1]))

        with self.assertRaisesRegex(AssertionError,
                                    "target authority reason"):
            self.assert_post_cleanup_right_change_rejects_receipt(
                select_same_text_without_context_drift, lambda: None,
                expected_authority_reason="selection-identity", typed_rejection=True)
        self.assertFalse(bypass.exists(), "only the identity guard was bypassed")
        self.assertTrue(
            pathlib.Path("/run/punto-e2e-cleanup-receipt-retained").exists(),
            "bypassing only identity validation makes the control fail",
        )
        self.assertFalse(
            pathlib.Path("/run/punto-e2e-cleanup-authority-timeout").exists(),
            "identity strength control did not fail by timeout",
        )
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "identity strength final DOM state")
        self.assertEqual(self.browser_state()[:7],
                         ["ghbdtn", 6, 6, "ghbdtn", 0, 6, "b"])
        self.assertEqual(self.browser_state()[-2], [1, 1])
        self.assertEqual(self.plain_clipboard_snapshot(), clipboard)
        self.assertEqual(self.dispatches(), 0)
        self.assertEqual(self.keyboard_group(), 0)
        self.assertEqual(self.layout_shortcut.activations, 0)
        for key in (gtk.KEY_PAUSE, gtk.KEY_RIGHT, gtk.KEY_LEFTCTRL,
                    gtk.KEY_LEFTSHIFT, gtk.KEY_LEFTALT, gtk.KEY_GRAVE):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())

    def test_session_control_fails_when_lease_revocation_is_bypassed(self):
        revoke = pathlib.Path("/run/punto-e2e-revoke-cleanup-lease")
        commanded = pathlib.Path(
            "/run/punto-e2e-cleanup-lease-revocation-commanded"
        )
        bypass = pathlib.Path(
            "/run/punto-e2e-bypass-cleanup-lease-revocation"
        )
        for marker in (revoke, commanded, bypass):
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        bypass.touch(mode=0o600)

        def request_revocation():
            revoke.touch(mode=0o600)
            self.pump_until(commanded.exists,
                            "bypassed revocation command observed")

        with self.assertRaisesRegex(AssertionError,
                                    "target authority reason"):
            self.assert_post_cleanup_right_change_rejects_receipt(
                request_revocation, lambda: None,
                expected_authority_reason="session")
        self.assertTrue(
            pathlib.Path("/run/punto-e2e-cleanup-receipt-retained").exists(),
            "bypassing only lease revocation makes the control fail",
        )
        self.assertFalse(
            pathlib.Path("/run/punto-e2e-cleanup-authority-timeout").exists(),
            "session strength control did not fail by timeout",
        )

    def test_missing_layout_shortcut_rejects_before_text_replacement(self):
        self.layout_shortcut.stop()
        self.layout_shortcut = gtk.DesktopLayoutShortcut(
            self.x11.display, captured=False)
        self.layout_shortcut.start()
        self.addCleanup(self.layout_shortcut.stop)
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "browser typed source")
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            "missing layout shortcut rejection",
        )
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after missing shortcut rejection")
        self.assertEqual(self.browser_state()[:3], ["ghbdtn", 6, 6])
        self.assertEqual(self.keyboard_group(), 0, self.harness.diagnostic())
        self.assertEqual(self.dispatches(), 0)

    def test_physical_layout_shortcut_preserves_next_correction(self):
        daemon_pid = self.harness.process.pid
        self.first_manual_conversion()

        activations = self.layout_shortcut.activations
        self.send_chord((gtk.KEY_LEFTCTRL,), key=gtk.KEY_GRAVE)
        self.pump_until(
            lambda: self.layout_shortcut.activations == activations + 1
            and self.keyboard_group() == 0,
            "physical desktop layout shortcut",
        )
        self.pump_for(0.05)
        self.assertEqual(self.layout_shortcut.activations, activations + 1)
        self.assertFalse(self.key_is_down(gtk.KEY_LEFTCTRL))
        self.assertFalse(self.key_is_down(gtk.KEY_GRAVE))
        activations = self.layout_shortcut.activations
        self.harness.send_events([
            (gtk.EV_KEY, gtk.KEY_LEFTCTRL, 1), (gtk.EV_SYN, gtk.SYN_REPORT, 0),
            (gtk.EV_KEY, gtk.KEY_GRAVE, 2), (gtk.EV_SYN, gtk.SYN_REPORT, 0),
            (gtk.EV_KEY, gtk.KEY_GRAVE, 0), (gtk.EV_SYN, gtk.SYN_REPORT, 0),
            (gtk.EV_KEY, gtk.KEY_LEFTCTRL, 0), (gtk.EV_SYN, gtk.SYN_REPORT, 0),
        ])
        self.pump_until(
            lambda: self.layout_shortcut.activations == activations + 1
            and self.keyboard_group() == 1,
            "repeated configured layout shortcut press",
        )
        self.pump_for(0.05)
        self.assertEqual(self.layout_shortcut.activations, activations + 1)
        self.assertFalse(self.key_is_down(gtk.KEY_LEFTCTRL))
        self.assertFalse(self.key_is_down(gtk.KEY_GRAVE))
        activations = self.layout_shortcut.activations
        self.send_chord((gtk.KEY_LEFTCTRL,), key=gtk.KEY_GRAVE)
        self.pump_until(lambda: self.layout_shortcut.activations == activations + 1
                        and self.keyboard_group() == 0,
                        "configured layout shortcut after repeat")
        self.pump_for(0.05)
        self.assertEqual(self.layout_shortcut.activations, activations + 1)
        self.assertFalse(self.key_is_down(gtk.KEY_LEFTCTRL))
        self.assertFalse(self.key_is_down(gtk.KEY_GRAVE))
        self.harness.send_key(gtk.KEY_SPACE)
        self.harness.type_word("ghbdtn")
        self.pump_until(
            lambda: self.browser_state()[:3] == ["привет ghbdtn", 13, 13],
            "fresh source after physical layout shortcut",
        )
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.browser_state()[:3] == ["привет привет", 13, 13],
            "correction after physical layout shortcut",
        )
        self.assertEqual(self.dispatches(), 2)
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.desktop_source_role(), 1)
        self.harness.type_word("f")
        self.pump_until(
            lambda: self.browser_state()[:3] == ["привет привета", 14, 14],
            "next physical character uses corrected Russian layout",
        )

    def test_wrong_modifier_side_invalidates_retained_receipt(self):
        self.first_manual_conversion()
        clipboard = self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD)
        activations = self.layout_shortcut.activations
        self.send_chord((gtk.KEY_RIGHTCTRL,), key=gtk.KEY_GRAVE)
        self.pump_until(
            lambda: self.layout_shortcut.activations == activations + 1
            and self.keyboard_group() == 0,
            "desktop shortcut from unconfigured Control side",
        )
        self.harness.type_word("ghbdtn")
        self.pump_until(
            lambda: self.browser_state()[:3] == ["приветghbdtn", 12, 12],
            "fresh candidate after wrong-side shortcut",
        )
        self.select_field_b_word()
        self.assert_pause_rejected(
            preflight=True,
            expected_stage=("editor_context", "retained_selection_untrusted"),
        )
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD), clipboard)
        for key in (gtk.KEY_LEFTCTRL, gtk.KEY_RIGHTCTRL, gtk.KEY_GRAVE,
                    gtk.KEY_LEFTSHIFT, gtk.KEY_LEFTALT, gtk.KEY_PAUSE):
            self.assertFalse(self.key_is_down(key))

    def test_extra_modifier_invalidates_retained_receipt(self):
        self.first_manual_conversion()
        activations = self.layout_shortcut.activations
        self.send_chord((gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT), key=gtk.KEY_GRAVE)
        self.pump_for(0.03)
        self.assertEqual(self.layout_shortcut.activations, activations)
        self.assertEqual(self.keyboard_group(), 1)

        self.send_chord((gtk.KEY_LEFTCTRL,), key=gtk.KEY_GRAVE)
        self.pump_until(lambda: self.keyboard_group() == 0,
                        "layout reset after extra-modifier chord")
        self.harness.type_word("ghbdtn")
        self.pump_until(
            lambda: self.browser_state()[:3] == ["приветghbdtn", 12, 12],
            "fresh candidate after extra-modifier shortcut",
        )
        self.select_field_b_word()
        self.assert_pause_rejected(
            preflight=True,
            expected_stage=("editor_context", "retained_selection_untrusted"),
        )

    def test_unhandled_layout_shortcut_rejects_before_text_replacement(self):
        self.layout_shortcut.enabled = False
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "browser typed source")
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            "unhandled layout shortcut rejection",
        )
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after unhandled shortcut rejection")
        self.assertEqual(self.browser_state()[:3], ["ghbdtn", 6, 6])
        self.assertEqual(self.layout_shortcut.activations, 0)
        self.assertEqual(self.keyboard_group(), 0)
        self.assertEqual(self.dispatches(), 0)

    def test_retained_receipt_survives_safe_layout_rejection(self):
        self.first_manual_conversion()
        daemon_pid = self.harness.process.pid
        daemon_start = gtk.process_start_time(daemon_pid)
        activations = self.layout_shortcut.activations
        group = self.keyboard_group()
        clipboard = self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD)
        marker = pathlib.Path("/run/punto-e2e-force-layout-bridge-reject")
        marker.unlink(missing_ok=True)
        self.addCleanup(marker.unlink, missing_ok=True)
        marker.touch(mode=0o600)
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            "safe repeated correction rejection",
        )
        self.assertFalse(marker.exists(), "typed layout rejection was consumed")
        self.assertIn("rejection_stage=layout_bridge_test_rejected",
                      self.harness.diagnostic())
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after safe repeated rejection")
        self.assertEqual(self.browser_state()[:3], ["привет", 6, 6])
        self.assertEqual(self.dispatches(), 1)
        self.assertEqual(self.layout_shortcut.activations, activations)
        self.assertEqual(self.keyboard_group(), group)
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD), clipboard)
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "retained receipt recovery without restart")
        self.assertEqual(self.dispatches(), 2)
        self.assertEqual(self.layout_shortcut.activations, activations + 1)
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD), clipboard)
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(gtk.process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())

    def assert_transport_fault_invalidates_retained_receipt(self, marker_name):
        authority = (self.harness.process.pid,
                     gtk.process_start_time(self.harness.process.pid),
                     self.clipboard_owner(),
                     self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD))
        self.first_manual_conversion()
        marker = pathlib.Path("/run", marker_name)
        marker.unlink(missing_ok=True)
        self.addCleanup(marker.unlink, missing_ok=True)
        marker.touch(mode=0o600)
        status = 0
        rejected = self.harness.diagnostic().count(
            f"Word edit dispatch status={status}")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                f"Word edit dispatch status={status}") > rejected,
            f"{marker_name} rejects retained correction",
        )
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, f"fresh DOM after {marker_name}")
        self.assertEqual(self.dispatches(), 1)
        self.assertFalse(marker.exists(), f"{marker_name} fault was not reached")
        self.assert_pause_rejected()
        self.assertEqual(self.dispatches(), 1)
        self.assert_pause_rejected()
        self.assert_fresh_physical_manual_recovery(authority)

    def test_keymap_failure_invalidates_retained_receipt(self):
        self.assert_transport_fault_invalidates_retained_receipt(
            "punto-e2e-fail-word-keymap")

    def assert_layout_ack_failure_recovers(self):
        daemon_pid = self.harness.process.pid
        self.first_manual_conversion()
        fault = pathlib.Path("/run/punto-e2e-fail-layout-hotkey-send")
        ack = pathlib.Path("/run/punto-e2e-layout-hotkey-ack-fault-consumed")
        for marker in (fault, ack):
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        fault.touch(mode=0o600)
        rejected = self.full_diagnostic().count("Word edit dispatch status=0")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.full_diagnostic().count(
            "Word edit dispatch status=0") > rejected,
            "failed layout acknowledgment refuses replacement after exact cleanup")
        self.wait_for_fresh_dom(self.browser_state()[-1], "fresh safe-refusal DOM")
        self.assertFalse(fault.exists())
        self.assertTrue(ack.exists(), "checked layout acknowledgment fault was injected")
        self.assertEqual(self.browser_state()[:3], ["привет", 6, 6])
        self.assertEqual(self.dispatches(), 1)
        self.assertEqual(self.keyboard_group(), 0)
        self.assertFalse(self.key_is_down(self.layout_shortcut.modifier))
        self.assertFalse(self.key_is_down(self.layout_shortcut.key))
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[:3] == ["приветf", 7, 7],
                        "next physical character follows the observed layout")
        self.harness.send_key(gtk.KEY_SPACE)
        self.harness.type_word("ghbdtn")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.browser_state()[:3] ==
            ["приветf привет", 14, 14],
            "fresh correction recovers after failed acknowledgment without restart",
        )
        self.assertEqual(self.dispatches(), 2)
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[:3] == ["приветf привета", 15, 15],
                        "recovered correction sets Russian for subsequent input")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertIsNone(self.harness.process.poll())

    def test_layout_hotkey_send_failure_invalidates_retained_receipt(self):
        self.assert_layout_ack_failure_recovers()

    def test_twenty_six_initial_failures_recover_from_zero_dispatches(self):
        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)
        self.layout_shortcut.enabled = False
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "initial source before zero-dispatch failures")
        for attempt in range(26):
            with self.subTest(attempt=attempt + 1):
                marker = pathlib.Path(
                    "/run/punto-e2e-force-layout-bridge-reject")
                marker.touch(mode=0o600)
                self.addCleanup(marker.unlink, missing_ok=True)
                self.assert_pause_rejected()
                self.assertFalse(marker.exists())
                self.assertEqual(self.dispatches(), 0)
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == ["привет", 6, 6],
                        "recovery after zero-dispatch failure series")
        self.assertEqual(self.dispatches(), 1)
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(process_start_time(daemon_pid), daemon_start)

    def assert_context_change_invalidates_retained_receipt(self, change, restore):
        self.first_manual_conversion()
        activations = self.layout_shortcut.activations
        self.layout_shortcut.arm_blocked_activation()
        self.addCleanup(self.layout_shortcut.permit_activation.set)
        before = self.browser_state()
        rejected = self.harness.diagnostic().count("Word edit dispatch status=1")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(self.layout_shortcut.activation_started.is_set,
                        "retained correction entered layout preflight")
        change()
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=1") > rejected,
            "context change rejects retained correction",
        )
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after context rejection")
        after = self.browser_state()
        self.assertEqual(after[0], before[0])
        self.assertEqual(after[3:6], before[3:6])
        self.assertEqual(after[1:3], [0, 6])
        self.assertEqual(self.dispatches(), 1)

        self.layout_shortcut.permit_activation.set()
        self.pump_until(
            lambda: self.layout_shortcut.activations == activations + 1
            and self.keyboard_group() == 0,
            "blocked desktop activation completed",
        )
        restore()
        self.assert_stale_primary_refuses_then_fresh_word_recovers()

    def test_pointer_change_invalidates_retained_receipt(self):
        original = self.pointer_position()
        self.assert_context_change_invalidates_retained_receipt(
            lambda: self.xdo("mousemove", str(original[0] + 40), str(original[1])),
            lambda: self.xdo("mousemove", str(original[0]), str(original[1])),
        )

    def test_focus_change_invalidates_retained_receipt(self):
        gtk_window = self.window.get_window().get_xid()
        self.assert_context_change_invalidates_retained_receipt(
            lambda: self.xdo("windowfocus", "--sync", str(gtk_window)),
            lambda: (self.xdo("windowfocus", "--sync", str(self.browser_window)),
                     self.publish_active_window(self.browser_window)),
        )

    def test_lock_change_invalidates_retained_receipt(self):
        self.assert_context_change_invalidates_retained_receipt(
            lambda: self.keyboard_locks(2),
            lambda: self.keyboard_locks(0),
        )

    def test_new_selection_after_safe_rejection_invalidates_old_receipt(self):
        authority = (self.harness.process.pid,
                     gtk.process_start_time(self.harness.process.pid),
                     self.clipboard_owner(),
                     self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD))
        self.first_manual_conversion()
        self.layout_shortcut.enabled = False
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            "safe rejection before genuine replacement selection",
        )
        self.layout_shortcut.enabled = True

        left, top, decoration = self.browser_state()[8]
        self.xdo("mousemove", "--window", str(self.browser_window),
                 str(int(left + 30)), str(int(top + decoration + 20)))
        self.xdo("click", "--repeat", "2", "--delay", "100", "1")
        self.pump_until(lambda: self.browser_state()[3:7] ==
                        ["ghbdtn", 0, 6, "b"],
                        "genuine same-text selection after safe rejection")
        self.xdo("mousemove", "--window", str(self.browser_window),
                 str(int(left + 30)), str(int(top + decoration - 40)))
        self.xdo("click", "1")
        self.pump_until(lambda: self.browser_state()[6] == "a",
                        "original browser field refocused")
        before = self.browser_state()
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            "new same-text selection rejects old receipt",
        )
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after stale receipt rejection")
        self.assertEqual(self.browser_state()[:-1], before[:-1])
        self.assertEqual(self.dispatches(), 1)
        self.assert_pause_rejected(preflight=True)
        self.assert_fresh_physical_manual_recovery(authority)

    def test_selection_layout_stall_refuses_before_browser_replay(self):
        left, top, decoration = self.browser_state()[8]
        self.xdo("mousemove", "--window", str(self.browser_window),
                 str(int(left + 30)), str(int(top + decoration + 20)))
        self.xdo("click", "--repeat", "2", "--delay", "100", "1")
        self.pump_until(lambda: self.browser_state()[3:7] ==
                        ["ghbdtn", 0, 6, "b"],
                        "browser selection before layout preflight")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_PRIMARY), "ghbdtn")
        self.layout_shortcut.enabled = False
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.send_chord((gtk.KEY_LEFTSHIFT,))
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            "browser final layout transition failure",
        )
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after selection refusal")
        self.assertEqual(self.browser_state()[3:7], ["ghbdtn", 0, 6, "b"])
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.assertEqual(self.dispatches(), 0)

    def test_selection_collapsed_during_layout_rejects_retained_primary(self):
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "source before real user selection")
        self.xdo("key", "ctrl+a")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 0, 6],
                        "actual selected source before layout transition")
        clipboard = self.plain_clipboard_snapshot()
        markers = {name: pathlib.Path("/run", f"punto-e2e-{name}") for name in
                   ("arm-after-replay-layout", "after-replay-layout", "release-after-replay-layout")}
        for marker in markers.values():
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        self.addCleanup(markers["release-after-replay-layout"].touch, exist_ok=True)
        markers["arm-after-replay-layout"].touch(mode=0o600)
        rejected = self.full_diagnostic().count("Word edit dispatch status=0")
        self.send_chord((gtk.KEY_LEFTSHIFT,))
        self.pump_until(markers["after-replay-layout"].exists,
                        "selection replay awaits actual desktop layout transition")
        self.xdo("key", "Right")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "user collapses selection while layout request is awaited")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_PRIMARY), "ghbdtn")
        markers["release-after-replay-layout"].touch(mode=0o600)
        self.pump_until(lambda: self.full_diagnostic().count("Word edit dispatch status=0") > rejected,
                        "stale selected range refuses before first write")
        self.assertIn("rejection_stage=selection_source_range", self.full_diagnostic())
        self.wait_for_fresh_dom(self.browser_state()[-1], "fresh DOM after collapsed-range refusal")
        self.assertEqual(self.browser_state()[:3], ["ghbdtn", 6, 6])
        self.assertEqual(self.dispatches(), 0)
        self.assertEqual(self.plain_clipboard_snapshot(), clipboard)
        self.xdo("key", "ctrl+a")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 0, 6],
                        "fresh recovery selection")
        self.send_chord((gtk.KEY_LEFTSHIFT,))
        self.pump_until(lambda: self.browser_state()[:3] == ["привет", 6, 6],
                        "fresh selection succeeds after layout-time collapse")
        self.pump_until(lambda: self.dispatches() == 1, "fresh selected operation dispatched")
        self.assertEqual(self.plain_clipboard_snapshot(), clipboard)
        self.assertEqual(self.output_count(gtk.KEY_PAUSE), 0)
        for key in (gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT, gtk.KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def real_layout_bridge_selection_ru_to_en_uses_reasserting_desktop_owner(self):
        self.set_selection(gtk.Gdk.SELECTION_CLIPBOARD, "руддщ suffix")
        self.send_chord((gtk.KEY_LEFTCTRL,), gtk.KEY_V)
        self.pump_until(lambda: self.browser_state()[:3] == ["руддщ suffix", 12, 12],
                        "ordinary browser subset source pasted")
        self.set_selection(gtk.Gdk.SELECTION_CLIPBOARD, "startup clipboard baseline")
        clipboard_before = self.plain_clipboard_snapshot()
        self.set_desktop_layout(1)
        self.assertIsNotNone(self.real_layout_bridge)
        self.enable_desktop_reassertion()
        self.xdo("key", "Home", "Right", "Right", "Right", "Right", "Right",
                 "shift+Home")
        self.pump_until(lambda: self.browser_state()[:3] == ["руддщ suffix", 0, 5],
                        "real browser subset selected in RU desktop source")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_PRIMARY), "руддщ")
        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)
        activations = self.desktop_activation_count()
        self.send_chord((gtk.KEY_LEFTSHIFT,))
        self.pump_until(lambda: self.browser_state()[:3] == ["hello suffix", 5, 5],
                        "browser subset correction survives desktop reassertion")
        self.pump_until(lambda: self.dispatches() == 1, "complete browser subset dispatch")
        self.wait_for_fresh_dom(self.browser_state()[-1], "fresh corrected subset receipt")
        self.assertEqual(self.desktop_source_role(), 0)
        self.assertEqual(self.keyboard_group(), 0)
        self.assertEqual(self.desktop_activation_count(), activations + 1)
        self.assertEqual(self.desktop_owner().source_reassertions, 0)
        self.harness.type_word("a")
        self.pump_until(lambda: self.browser_state()[:3] == ["helloa suffix", 6, 6],
                        "next browser symbol follows published EN source")
        self.assertEqual(self.plain_clipboard_snapshot(), clipboard_before)
        for key in (gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT, gtk.KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(process_start_time(daemon_pid), daemon_start)

    def test_plain_paste_accelerator_keeps_ru_layout(self):
        self.set_desktop_layout(1)
        self.layout_shortcut.enabled = False
        self.layout_shortcut.reassert_source = True
        self.set_selection(gtk.Gdk.SELECTION_CLIPBOARD, "paste probe")
        self.send_chord((gtk.KEY_LEFTCTRL,), gtk.KEY_V)
        self.pump_until(lambda: self.browser_state()[:3] == ["paste probe", 11, 11],
                        "physical Ctrl+V resolves Chromium accelerator in RU")
        self.wait_for_fresh_dom(self.browser_state()[-1], "fresh ordinary paste result")
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.layout_shortcut.activations, 0)
        self.assertEqual(self.layout_shortcut.source_reassertions, 0)
        self.assertEqual(self.dispatches(), 0)
        self.assertFalse(self.key_is_down(gtk.KEY_LEFTCTRL))

    def test_plain_clipboard_selection_layout_completes_without_primary_change(self):
        left, top, decoration = self.browser_state()[8]
        self.xdo("mousemove", "--window", str(self.browser_window),
                 str(int(left + 30)), str(int(top + decoration + 20)))
        self.xdo("click", "--repeat", "2", "--delay", "100", "1")
        self.pump_until(lambda: self.browser_state()[3:7] ==
                        ["ghbdtn", 0, 6, "b"],
                        "plain clipboard browser selection")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.send_chord((gtk.KEY_LEFTSHIFT,))
        self.pump_until(lambda: self.browser_state()[3:7] ==
                        ["привет", 6, 6, "b"],
                        "plain clipboard selection replacement")
        self.pump_until(lambda: self.dispatches() == 1,
                        "plain clipboard complete dispatch")
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[3:7] ==
                        ["привета", 7, 7, "b"],
                        "next browser key uses completed layout")

    def test_selection_layout_preserves_chromium_clipboard_metadata(self):
        left, top, decoration = self.browser_state()[8]
        self.xdo("mousemove", "--window", str(self.browser_window),
                 str(int(left + 30)), str(int(top + decoration + 20)))
        self.xdo("click", "--repeat", "2", "--delay", "100", "1")
        self.pump_until(lambda: self.browser_state()[3:7] ==
                        ["ghbdtn", 0, 6, "b"],
                        "browser selection before rich clipboard copy")
        self.xdo("key", "ctrl+c")
        self.pump_until(
            lambda: self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD) == "ghbdtn",
            "Chromium clipboard payload",
        )
        clipboard_before = self.rich_clipboard_snapshot()
        self.assertTrue(
            any(target.startswith("chromium/x-")
                for target in clipboard_before[1]),
            clipboard_before[1],
        )
        self.send_chord((gtk.KEY_LEFTSHIFT,))
        self.pump_until(lambda: self.browser_state()[3:7] ==
                        ["привет", 6, 6, "b"],
                        "browser selection correction with rich clipboard")
        self.pump_until(lambda: self.dispatches() == 1,
                        "rich clipboard selection final layout dispatch")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         "ghbdtn")
        self.assertEqual(self.rich_clipboard_snapshot(), clipboard_before)
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertEqual(self.keyboard_group(), 1)

        self.harness.send_key(gtk.KEY_SPACE)
        self.harness.type_word("hello")
        self.pump_until(lambda: self.browser_state()[3:7] ==
                        ["привет руддщ", 12, 12, "b"],
                        "new wrong-layout word after rich replay")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[3:7] ==
                        ["привет hello", 12, 12, "b"],
                        "next correction admitted by exact rich replay receipt")
        self.pump_until(lambda: self.dispatches() == 2,
                        "second dispatch after rich replay")
        self.send_chord((gtk.KEY_LEFTCTRL,), gtk.KEY_Z)
        self.pump_until(lambda: self.browser_state()[3:7] ==
                        ["привет руддщ", 12, 12, "b"],
                        "undo after rich replay continuation")

    def test_rich_clipboard_selection_keeps_left_duplicate_range(self):
        left, top, decoration = self.browser_state()[8]
        self.xdo("mousemove", "--window", str(self.browser_window),
                 str(int(left + 30)), str(int(top + decoration + 20)))
        self.xdo("click", "1")
        self.set_selection(gtk.Gdk.SELECTION_CLIPBOARD, "aa suffix")
        self.xdo("key", "ctrl+a")
        self.xdo("key", "ctrl+v")
        self.pump_until(lambda: self.browser_state()[3:7] ==
                        ["aa suffix", 9, 9, "b"],
                        "duplicate-rich source inserted")
        self.xdo("key", "Home")
        self.xdo("key", "shift+Right")
        self.pump_until(lambda: self.browser_state()[3:7] ==
                        ["aa suffix", 0, 1, "b"],
                        "exact left duplicate selected")
        self.xdo("key", "ctrl+c")
        self.pump_until(
            lambda: self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD) == "a",
            "left duplicate copied with rich targets",
        )
        clipboard_before = self.rich_clipboard_snapshot()
        self.send_chord((gtk.KEY_LEFTSHIFT,))
        self.pump_until(lambda: self.browser_state()[3:7] ==
                        ["фa suffix", 1, 1, "b"],
                        "rich replay changed only the original duplicate")
        self.pump_until(lambda: self.dispatches() == 1,
                        "rich duplicate complete dispatch")
        self.assertEqual(self.rich_clipboard_snapshot(), clipboard_before)

    def prepare_rich_browser_selection(self, source="ghbdtn", source_group=0):
        left, top, decoration = self.browser_state()[8]
        self.xdo("mousemove", "--window", str(self.browser_window),
                 str(int(left + 30)), str(int(top + decoration + 20)))
        self.xdo("click", "--repeat", "2", "--delay", "100", "1")
        if source != "ghbdtn":
            self.set_selection(gtk.Gdk.SELECTION_CLIPBOARD, source)
            self.xdo("key", "BackSpace")
            self.xdo("key", "ctrl+v")
            self.pump_until(lambda: self.browser_state()[3:7] ==
                            [source, len(source), len(source), "b"],
                            "custom rich-selection source inserted")
            self.xdo("key", "ctrl+a")
        self.set_desktop_layout(source_group)
        self.pump_until(lambda: self.browser_state()[3:7] ==
                        [source, 0, len(source), "b"],
                        "browser selection before desktop-hop fault")
        self.xdo("key", "ctrl+c")
        self.pump_until(
            lambda: self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD) == source,
            "rich clipboard before desktop-hop fault",
        )
        return self.rich_clipboard_snapshot()

    def assert_replay_hop_failure_keeps_published_source(self, marker_name,
                                                       status=3):
        clipboard_before = self.prepare_rich_browser_selection()
        marker = pathlib.Path("/run", marker_name)
        marker.unlink(missing_ok=True)
        marker.touch(mode=0o600)
        self.addCleanup(marker.unlink, missing_ok=True)
        status_line = f"Word edit dispatch status={status}"
        rejected = self.harness.diagnostic().count(status_line)
        self.send_chord((gtk.KEY_LEFTSHIFT,))
        self.pump_until(
            lambda: self.harness.diagnostic().count(status_line) > rejected,
            "partial replay fault observed",
        )
        self.assertFalse(marker.exists())
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertEqual(self.keyboard_group(), 1, self.harness.diagnostic())
        self.assertEqual(self.rich_clipboard_snapshot(), clipboard_before)
        if status == 0:
            self.assertEqual(self.browser_state()[3:7], ["ghbdtn", 0, 6, "b"])
            self.assertEqual(self.dispatches(), 0)
        left, top, decoration = self.browser_state()[8]
        self.xdo("mousemove", "--window", str(self.browser_window),
                 str(int(left + 250)), str(int(top + decoration + 20)))
        self.xdo("click", "1")
        self.pump_until(lambda: self.browser_state()[6] == "b",
                        "browser target refocused after desktop-hop fault")
        self.harness.type_word("a")
        self.pump_until(lambda: self.browser_state()[6] == "b" and
                        self.browser_state()[3].endswith("ф"),
                        "next physical key uses observed Russian source")

    def test_replay_failure_after_desktop_hop_keeps_coherent_target(self):
        self.assert_replay_hop_failure_keeps_published_source(
            "punto-e2e-fail-after-replay-layout")

    def test_replay_failure_after_first_stroke_keeps_coherent_target(self):
        self.assert_replay_hop_failure_keeps_published_source(
            "punto-e2e-fail-after-first-replay-stroke")

    def test_replay_hop_hard_deadline_refuses_without_rescue(self):
        self.assert_replay_hop_failure_keeps_published_source(
            "punto-e2e-expire-after-replay-layout", status=0)

    def test_replay_hop_cancellation_preserves_published_source(self):
        clipboard_before = self.prepare_rich_browser_selection()
        markers = {
            name: pathlib.Path("/run", "punto-e2e-" + name)
            for name in ("arm-after-replay-layout", "after-replay-layout",
                         "release-after-replay-layout")
        }
        for marker in markers.values():
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        self.addCleanup(markers["release-after-replay-layout"].touch,
                        exist_ok=True)
        markers["arm-after-replay-layout"].touch(mode=0o600)
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.send_chord((gtk.KEY_LEFTSHIFT,))
        self.pump_until(markers["after-replay-layout"].exists,
                        "desktop hop reached before cancellation")
        with gtk.socket.socket(gtk.socket.AF_UNIX, gtk.socket.SOCK_STREAM) as client:
            client.settimeout(gtk.EVENT_TIMEOUT)
            client.connect("/run/punto.sock")
            client.sendall(b"SET_STATUS 0\n")
            self.pump_for(0.05)
            markers["release-after-replay-layout"].touch(mode=0o600)
            self.assertEqual(client.recv(8192), b"OK DISABLED\n")
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            "queued mutation cancelled replay hop",
        )
        self.assertEqual(self.desktop_source_role(), 1)
        self.assertEqual(self.keyboard_group(), 1, self.harness.diagnostic())
        self.assertEqual(self.rich_clipboard_snapshot(), clipboard_before)




    def test_delayed_clipboard_initialization_uses_remaining_macro_budget(self):
        marker = pathlib.Path("/run/punto-e2e-slow-clipboard-init")
        marker.touch(mode=0o600)
        self.first_manual_conversion()
        self.assertFalse(marker.exists(), "clipboard initialization fault was not reached")

    def test_consecutive_automatic_corrections(self):
        self.assertEqual(gtk.ipc_request(b"SET_STATUS 1\n"), b"OK ENABLED\n")
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "first browser source word")
        self.harness.send_key(gtk.KEY_SPACE)
        self.pump_until(lambda: self.browser_state()[:3] == ["привет ", 7, 7],
                        "first browser automatic correction")
        self.harness.type_word("hello")
        self.pump_until(lambda: self.browser_state()[:3] == ["привет руддщ", 12, 12],
                        "second browser source word")
        self.harness.send_key(gtk.KEY_SPACE)
        self.pump_until(lambda: self.browser_state()[:3] == ["привет hello ", 13, 13],
                        "second browser automatic correction")
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[:3] == ["привет hello f", 14, 14],
                        "corrected English layout reaches native browser")
        self.assertEqual(self.dispatches(), 2)

    def test_first_automatic_word_after_owned_browser_reopen(self):
        daemon_pid = self.harness.process.pid
        daemon_start = process_start_time(daemon_pid)
        helper_pid = self.session_bridge_process.pid
        helper_start = process_start_time(helper_pid)
        clipboard = self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD)
        clipboard_owner = self.clipboard_owner()
        self.assertEqual(gtk.ipc_request(b"SET_STATUS 1\n"), b"OK ENABLED\n")
        self.harness.type_word("ghbdtn")
        self.harness.send_key(gtk.KEY_SPACE)
        self.pump_until(lambda: self.browser_state()[:3] == ["привет ", 7, 7],
                        "old browser automatic correction establishes history")
        self.pump_until(lambda: self.dispatches() == 1, "old browser dispatch")
        self.assertGreater(int(self.stats_fields()[1]["history_words"]), 0)

        for physical, source, corrected, group, following in (
            ("hello", "руддщ", "hello", 0, "f"),
            ("ghbdtn", "ghbdtn", "привет", 1, "а"),
        ):
            old_browser_pid = self.browser.pid
            arguments = self.browser.args
            self.stop_browser()
            self.assertIsNotNone(self.browser.poll())
            self.assertFalse(browser_group_has_live_members(old_browser_pid))
            self.browser = subprocess.Popen(
                arguments, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=self.browser_log, start_new_session=True,
            )
            self.assertNotEqual(self.browser.pid, old_browser_pid)
            self.browser_window = None

            def find_replacement():
                self.assertIsNone(self.browser.poll(), "replacement Chromium exited")
                result = subprocess.run(
                    ["xdotool", "search", "--name", r"^PUNTO%5B"],
                    capture_output=True, text=True, timeout=2,
                )
                windows = result.stdout.splitlines()
                if windows:
                    self.browser_window = int(windows[0])
                return self.browser_window is not None

            self.pump_until(find_replacement, "replacement Chromium window", timeout=45)
            self.xdo("windowfocus", "--sync", str(self.browser_window))
            self.publish_active_window(self.browser_window)
            self.pump_until(lambda: self.browser_state()[:3] == ["", 0, 0],
                            "replacement browser starts empty")
            diagnostic_start = len(self.full_diagnostic())
            dispatches = self.dispatches()
            self.harness.type_word(physical)
            self.pump_until(lambda: self.browser_state()[:3] ==
                            [source, len(source), len(source)],
                            "first physical word reaches replacement browser")
            self.harness.send_key(gtk.KEY_SPACE)
            expected = corrected + " "
            self.pump_until(lambda: self.browser_state()[:3] ==
                            [expected, len(expected), len(expected)],
                            "first replacement automatic correction")
            self.pump_until(lambda: self.dispatches() == dispatches + 1,
                            "replacement dispatch completes once")
            self.assertIn("present=1", "\n".join(
                line for line in self.full_diagnostic()[diagnostic_start:].splitlines()
                if "origin_capture_result" in line))
            self.assertEqual(self.keyboard_group(), group)
            self.assertEqual(self.desktop_source_role(), group)
            self.harness.type_word("f")
            expected += following
            self.pump_until(lambda: self.browser_state()[:3] ==
                            [expected, len(expected), len(expected)],
                            "next glyph follows replacement correction source")
            self.wait_for_fresh_dom(self.browser_state()[-1], "fresh replacement DOM")
            self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD), clipboard)
            self.assertEqual(self.clipboard_owner(), clipboard_owner)
            for key in (gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT, gtk.KEY_LEFTALT,
                        gtk.KEY_PAUSE):
                self.assertFalse(self.key_is_down(key))
            self.assertEqual(self.harness.process.pid, daemon_pid)
            self.assertEqual(process_start_time(daemon_pid), daemon_start)
            self.assertIsNone(self.harness.process.poll())
            self.assertEqual(self.session_bridge_process.pid, helper_pid)
            self.assertEqual(process_start_time(helper_pid), helper_start)
            self.assertIsNone(self.session_bridge_process.poll())

    def test_automatic_correction_retries_once_after_fresh_input(self):
        self.assertEqual(gtk.ipc_request(b"SET_STATUS 1\n"), b"OK ENABLED\n")
        self.harness.type_word("ghbdtn")
        self.harness.send_key(gtk.KEY_SPACE)
        self.pump_until(
            lambda: self.browser_state()[:3] == ["привет ", 7, 7],
            "first automatic correction",
        )
        self.layout_shortcut.enabled = False
        self.harness.type_word("hello")
        self.harness.send_key(gtk.KEY_SPACE)
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            "automatic correction rejected while desktop shortcut is unavailable",
        )
        self.pump_until(
            lambda: self.browser_state()[:3] == ["привет руддщ ", 13, 13],
            "failed automatic correction collapses its prepared selection",
        )
        self.assertEqual(self.dispatches(), 1)

        self.layout_shortcut.enabled = True
        self.harness.type_word("ghbdtn")
        self.harness.send_key(gtk.KEY_SPACE)
        self.pump_until(
            lambda: self.browser_state()[:3] ==
            ["привет hello привет ", 20, 20],
            "single automatic retry after a later completed word",
        )
        self.assertEqual(self.dispatches(), 2)

    def test_automatic_retry_budget_does_not_loop_or_replay_later(self):
        daemon_pid = self.harness.process.pid
        daemon_start = gtk.process_start_time(daemon_pid)
        self.assertEqual(gtk.ipc_request(b"SET_STATUS 1\n"), b"OK ENABLED\n")
        self.harness.type_word("ghbdtn")
        self.harness.send_key(gtk.KEY_SPACE)
        self.pump_until(
            lambda: self.browser_state()[:3] == ["привет ", 7, 7],
            "first automatic correction before retry budget control",
        )
        initial_fault = pathlib.Path("/run/punto-e2e-force-layout-bridge-reject")
        initial_fault.unlink(missing_ok=True)
        self.addCleanup(initial_fault.unlink, missing_ok=True)
        initial_fault.touch(mode=0o600)
        rejection = (
            "Word edit dispatch status=0 rejection_stage=layout_bridge_test_rejected retry_policy=1")
        rejected = self.full_diagnostic().count(rejection)
        self.harness.type_word("hello")
        self.harness.send_key(gtk.KEY_SPACE)
        self.pump_until(
            lambda: self.full_diagnostic().count(rejection) > rejected,
            "initial automatic rejection",
        )
        self.assertFalse(initial_fault.exists(), "initial safe fault consumed")
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after typed automatic rejection")
        self.assertEqual(self.browser_state()[:3], ["привет руддщ ", 13, 13])
        self.assertEqual(self.dispatches(), 1)

        marker = pathlib.Path("/run/punto-e2e-fail-word-keymap")
        marker.unlink(missing_ok=True)
        marker.touch(mode=0o600)
        self.addCleanup(marker.unlink, missing_ok=True)
        attempts = self.harness.diagnostic().count("Word edit dispatch status=")
        self.harness.type_word("ghbdtn")
        self.harness.send_key(gtk.KEY_SPACE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=") > attempts,
            "single automatic retry is consumed",
        )
        self.pump_until(
            lambda: self.browser_state()[:3] ==
            ["привет руддщ привет ", 20, 20],
            "failed retry leaves exact editor text collapsed",
        )
        self.assertFalse(marker.exists(), "retry fault was not consumed")
        self.assertEqual(self.harness.diagnostic().count("Word edit dispatch status="),
                         attempts + 1, "exactly one automatic retry")

        attempts = self.harness.diagnostic().count("Word edit dispatch status=")
        self.harness.send_key(gtk.KEY_F8)
        self.pump_for(0.2)
        self.assertEqual(
            self.harness.diagnostic().count("Word edit dispatch status="),
            attempts,
        )
        self.assertEqual(self.dispatches(), 1)

        self.harness.type_word("ghbdtn")
        self.harness.send_key(gtk.KEY_SPACE)
        self.pump_until(
            lambda: self.browser_state()[:3] ==
            ["привет руддщ привет привет ", 27, 27],
            "later independent word after exhausted retry",
        )
        self.pump_for(0.2)
        self.assertEqual(self.browser_state()[:3],
                         ["привет руддщ привет привет ", 27, 27])
        self.assertEqual(self.dispatches(), 1)
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(gtk.process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())

    def test_new_same_text_selection_is_not_a_retained_receipt(self):
        daemon_pid = self.harness.process.pid
        daemon_start = gtk.process_start_time(daemon_pid)
        self.first_manual_conversion()
        left, top, decoration = self.browser_state()[8]
        self.xdo("mousemove", "--window", str(self.browser_window),
                 str(int(left + 30)), str(int(top + decoration + 20)))
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[:3] == ["привета", 7, 7],
                        "later physical text input in browser field a")
        self.select_field_b_word()
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_PRIMARY), "ghbdtn")
        primary = self.selection_text(gtk.Gdk.SELECTION_PRIMARY)
        clipboard = self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD)
        admission_prefix = "[punto] Editor admission refused "
        admissions_before = sum(line.startswith(admission_prefix)
                                for line in self.full_diagnostic().splitlines())
        self.assert_pause_rejected(
            preflight=True,
            expected_stage="editor_context",
        )
        new_admissions = [line for line in self.full_diagnostic().splitlines()
                          if line.startswith(admission_prefix)][admissions_before:]
        self.assertTrue(new_admissions, "fresh editor admission refusal")
        self.assertTrue(all("status=2 " in line and "expected_token_present=1" in line
                            for line in new_admissions), new_admissions)
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_PRIMARY), primary)
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD), clipboard)
        self.assertEqual(self.dispatches(), 1)
        for key in (gtk.KEY_PAUSE, gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT, gtk.KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(gtk.process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())

        self.harness.send_key(gtk.KEY_RIGHT)
        self.pump_until(lambda: self.browser_state()[3:7] ==
                        ["ghbdtn", 6, 6, "b"],
                        "physical navigation clears obsolete word provenance")
        self.select_field_b_word()
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_PRIMARY), primary)
        self.assert_pause_rejected(
            preflight=True,
            expected_stage="retained_selection_untrusted",
        )
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_PRIMARY), primary)
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD), clipboard)
        self.assertEqual(self.dispatches(), 1)
        for key in (gtk.KEY_PAUSE, gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT, gtk.KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))
        self.assertEqual(self.harness.process.pid, daemon_pid)
        self.assertEqual(gtk.process_start_time(daemon_pid), daemon_start)
        self.assertIsNone(self.harness.process.poll())

    def test_unreadable_new_selection_rejects_retained_receipt_preflight(self):
        self.first_manual_conversion()
        left, top, decoration = self.browser_state()[8]
        self.xdo("mousemove", "--window", str(self.browser_window),
                 str(int(left + 30)), str(int(top + decoration + 20)))
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[:3] == ["привета", 7, 7],
                        "later physical text before unreadable selection")
        self.select_field_b_word(delay=1)
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_PRIMARY),
                         "ghbdtn")
        fail_read = pathlib.Path(
            "/run/punto-e2e-fail-next-primary-payload-read"
        )
        failed = pathlib.Path("/run/punto-e2e-primary-payload-read-failed")
        for marker in (fail_read, failed):
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        fail_read.touch(mode=0o600)
        before = self.browser_state()
        clipboard_before = self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD)
        admission_prefix = "[punto] Editor admission refused "
        admissions_before = sum(line.startswith(admission_prefix)
                                for line in self.full_diagnostic().splitlines())
        self.assert_pause_rejected(
            preflight=True,
            expected_stage="editor_context",
        )
        new_admissions = [line for line in self.full_diagnostic().splitlines()
                          if line.startswith(admission_prefix)][admissions_before:]
        self.assertTrue(new_admissions, "first refusal must carry fresh admission evidence")
        self.assertTrue(all("expected_token_present=1" in line for line in new_admissions),
                        new_admissions)
        self.assertTrue(fail_read.exists(), "wrong child must not read PRIMARY")
        self.assertFalse(failed.exists())
        self.assertEqual(self.browser_state()[:-1], before[:-1])
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         clipboard_before)

        self.harness.send_key(gtk.KEY_RIGHT)
        self.pump_until(lambda: self.browser_state()[3:7] ==
                        ["ghbdtn", 6, 6, "b"],
                        "physical navigation clears obsolete word provenance")
        self.select_field_b_word(delay=1)
        before = self.browser_state()
        self.assert_pause_rejected(
            preflight=True,
            expected_stage="retained_selection_unreadable",
        )
        self.assertFalse(fail_read.exists(), "PRIMARY fault was consumed")
        self.assertTrue(failed.exists(), "PRIMARY payload failure was injected")
        self.assertEqual(self.browser_state()[:-1], before[:-1])
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         clipboard_before)
        self.assertEqual(self.dispatches(), 1)

    def test_delayed_space_release_preserves_delimiter(self):
        self.assertEqual(gtk.ipc_request(b"SET_STATUS 1\n"), b"OK ENABLED\n")
        relay = self.harness.relay
        relay.arm_delayed_key_release(gtk.KEY_SPACE, .025)
        self.harness.type_word("ghbdtn")
        self.harness.send_key(gtk.KEY_SPACE)
        self.pump_until(relay.key_release_delivered.is_set, "25ms delayed Space release")
        self.assertGreaterEqual(relay.key_release_delivered_at - relay.key_release_blocked_at,
                                .025)
        self.pump_until(lambda: self.browser_state()[:3] == ["привет ", 7, 7],
                        "automatic correction preserves Space and caret after delayed keyup")
        self.assertEqual(self.dispatches(), 1)

    def test_held_space_expires_without_edit_or_forced_keyup(self):
        checked = self.arm_held_space()
        self.assert_held_space_rejected()
        self.assertTrue(checked.exists(), "rejection reached the real key-release check")
        self.pump_for(max(0, self.harness.relay.key_release_blocked_at + .350
                          - time.monotonic()))
        self.assertTrue(self.key_is_down(gtk.KEY_SPACE))
        self.release_held_space()
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == ["привет ", 7, 7],
                        "manual correction recovers after held-key rejection")
        self.assertEqual(self.dispatches(), 1)

    def test_held_space_wait_cancels_when_disabled(self):
        checked = self.arm_held_space()
        self.pump_until(checked.exists, "real key-release wait reached")
        self.assertTrue(self.key_is_down(gtk.KEY_SPACE))
        self.assertNotIn("Word edit dispatch status=", self.harness.diagnostic())
        started = time.monotonic()
        self.assertEqual(gtk.ipc_request(b"SET_STATUS 0\n"), b"OK DISABLED\n")
        self.assertLess(time.monotonic() - started, 3)
        self.assert_held_space_rejected()
        self.release_held_space()

    def test_held_space_wait_cancels_when_focus_changes(self):
        checked = self.arm_held_space()
        self.pump_until(checked.exists, "real key-release wait reached")
        self.assertTrue(self.key_is_down(gtk.KEY_SPACE))
        self.assertNotIn("Word edit dispatch status=", self.harness.diagnostic())
        xid = self.window.get_window().get_xid()
        self.xdo("windowfocus", "--sync", str(xid))
        self.publish_active_window(xid)
        self.assert_held_space_rejected()
        self.release_held_space()
        self.assertEqual(self.entry.get_text(), "")

    def assert_manual_after_control_reset(self, command, *, failed_reload=False):
        self.first_manual_conversion()
        clipboard_before = self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD)
        owner_before = self.clipboard_owner()
        generation = int(self.stats_fields()[1]["config_generation"])
        if failed_reload:
            config = pathlib.Path("/tmp/punto-home/.config/punto/config.yaml")
            original = config.read_text(encoding="utf-8")
            self.addCleanup(config.write_text, original, encoding="utf-8")
            config.write_text("auto_switch: [unterminated\n", encoding="utf-8")
        if command == b"RELOAD\n":
            self.assertEqual(gtk.ipc_request(command), b"OK Scheduled\n")
            self.pump_until(
                lambda: int(self.stats_fields()[1]["config_generation"]) > generation
                and self.stats_fields()[1]["config_pending"] == "0",
                "admitted reload reached terminal completion",
            )
            self.assertEqual(self.stats_fields()[1]["config_result"],
                             "error" if failed_reload else "ok")
        else:
            self.assertEqual(gtk.ipc_request(command), b"OK DISABLED\n")
            self.assertEqual(gtk.ipc_request(b"GET_STATUS\n"), b"OK DISABLED\n")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_PRIMARY), "ghbdtn")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "manual correction after control/config reset")
        self.pump_until(lambda: self.dispatches() == 2, "fresh range after reset")
        if command != b"RELOAD\n":
            self.assertEqual(gtk.ipc_request(b"SET_STATUS 1\n"), b"OK ENABLED\n")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == ["привет", 6, 6],
                        "another fresh manual correction after reset")
        self.pump_until(lambda: self.dispatches() == 3, "third fresh range")
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_PRIMARY), "ghbdtn")
        self.harness.type_word("f")
        self.pump_until(lambda: self.browser_state()[:3] == ["привета", 7, 7],
                        "next ordinary key follows actual target layout")
        # A different field's new same-text selection is not the saved identity.
        self.select_field_b_word()
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_PRIMARY), "ghbdtn")
        self.assert_pause_rejected(preflight=True,
                                   expected_stage=("editor_context",
                                                   "retained_selection_untrusted"))
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_PRIMARY), "ghbdtn")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         clipboard_before)
        self.assertEqual(self.clipboard_owner(), owner_before)
        self.assertEqual(self.dispatches(), 3)
        self.assertEqual(self.output_count(gtk.KEY_PAUSE), 0)
        for key in (gtk.KEY_LEFTCTRL, gtk.KEY_LEFTSHIFT, gtk.KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def test_runtime_reset_preserves_manual_without_reusing_user_selection(self):
        self.assert_manual_after_control_reset(b"SET_STATUS 0\n")

    def test_completed_reload_preserves_manual_without_reusing_user_selection(self):
        self.assert_manual_after_control_reset(b"RELOAD\n")

    def test_failed_reload_preserves_manual_without_reusing_user_selection(self):
        self.assert_manual_after_control_reset(b"RELOAD\n", failed_reload=True)


class BrowserLifecycle(unittest.TestCase):
    def test_failure_diagnostics_precede_cleanup_without_changing_outcome(self):
        import contextlib
        import io
        from types import SimpleNamespace
        from unittest.mock import patch

        case = ChromiumE2E("test_held_space_expires_without_edit_or_forced_keyup")
        case.harness = SimpleNamespace(stderr=b"recorded rejection_stage=editor_origin_missing\n")
        for category in (None, "failures", "errors"):
            result = SimpleNamespace(failures=[], errors=[])
            if category:
                getattr(result, category).append((case, "original assertion traceback"))
            case._outcome = SimpleNamespace(result=result)
            output = io.StringIO()
            with self.subTest(category=category), contextlib.redirect_stderr(output), patch.object(
                    gtk.EventLoopGtkE2E, "tearDown") as cleanup:
                case.tearDown()
                cleanup.assert_called_once_with()
            if category:
                self.assertIn(case.id(), output.getvalue())
                self.assertIn("original assertion traceback", output.getvalue())
                self.assertIn("rejection_stage=editor_origin_missing", output.getvalue())
            else:
                self.assertEqual(output.getvalue(), "")

        case._outcome.result.failures = [(case, "original assertion traceback")]
        with patch("builtins.print", side_effect=OSError("observer output closed")), patch.object(
                gtk.EventLoopGtkE2E, "tearDown") as cleanup:
            with self.assertRaises(OSError):
                case.tearDown()
            cleanup.assert_called_once_with()

    def test_cleanup_stops_orphan_writer_and_preserves_unrelated_process(self):
        with tempfile.TemporaryDirectory(prefix="punto-browser-lifecycle-") as directory:
            profile = pathlib.Path(directory)
            writer = subprocess.Popen(
                [sys.executable, "-c", """
import os, pathlib, signal, sys, time
if os.fork():
    os._exit(0)
signal.signal(signal.SIGTERM, signal.SIG_IGN)
profile = pathlib.Path(sys.argv[1])
(profile / 'Default').mkdir()
(profile / 'writer.pid').write_text(str(os.getpid()))
while True:
    (profile / 'Default' / 'tick').write_text(str(time.monotonic_ns()))
    time.sleep(.002)
""", directory],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, start_new_session=True,
            )
            unrelated = subprocess.Popen(
                [sys.executable, "-c", "import time; time.sleep(30)"],
                start_new_session=True,
            )
            try:
                self.assertEqual(writer.wait(timeout=3), 0)
                tick = profile / "Default" / "tick"
                deadline = time.monotonic() + 3
                while not tick.exists() and time.monotonic() < deadline:
                    time.sleep(.01)
                self.assertTrue(tick.exists(), "orphan writer reached private profile")
                child = int((profile / "writer.pid").read_text())
                child_stat = pathlib.Path(f"/proc/{child}/stat")
                self.assertNotIn(process_status(child_stat)[0],
                                 ("Z", "X"), "writer is live after parent exit")
                case = ChromiumE2E("test_repeated_manual_conversion_with_retained_primary")
                case.browser = writer
                started = time.monotonic()
                case.stop_browser()
                self.assertLess(time.monotonic() - started, 7)
                status = process_status(child_stat)
                if status is not None:
                    self.assertIn(status[0],
                                  ("Z", "X"), "cleanup must stop the orphan writer")
                written = tick.stat().st_mtime_ns
                time.sleep(.05)
                self.assertEqual(tick.stat().st_mtime_ns, written,
                                 "profile is quiescent before directory removal")
                self.assertIsNone(unrelated.poll(), "unrelated process must survive")
                case.stop_browser()
            finally:
                try:
                    os.killpg(writer.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                writer.wait(timeout=3)
                unrelated.terminate()
                unrelated.wait(timeout=3)
                # The controlled RED must also stop its writer before rmtree.
                if "child_stat" in locals():
                    deadline = time.monotonic() + 3
                    while time.monotonic() < deadline:
                        status = process_status(child_stat)
                        if status is None or status[0] in ("Z", "X"):
                            break
                        time.sleep(.01)

    def test_cleanup_after_failed_startup_is_idempotent(self):
        process = subprocess.Popen(
            [sys.executable, "-c", "raise SystemExit(7)"], start_new_session=True,
        )
        self.assertEqual(process.wait(timeout=3), 7)
        case = ChromiumE2E("test_repeated_manual_conversion_with_retained_primary")
        case.browser = process
        case.stop_browser()
        case.stop_browser()

    def test_failed_stop_retains_profile_until_successful_cleanup(self):
        with tempfile.TemporaryDirectory(prefix="punto-browser-cleanup-failure-") as directory:
            case = ChromiumE2E("test_repeated_manual_conversion_with_retained_primary")
            case.profile = pathlib.Path(directory, "profile")
            case.profile.mkdir()
            sentinel = case.profile / "pending-write"
            sentinel.write_text("private fixture")
            case.browser = object()

            def fail_stop():
                raise RuntimeError("simulated private process stop failure")

            case.stop_browser = fail_stop
            with self.assertRaisesRegex(RuntimeError, "simulated private process stop failure"):
                case.cleanup_browser()
            self.assertEqual(sentinel.read_text(), "private fixture")
            # This also covers profile cleanup when Popen never created a child.
            case.browser = None
            case.cleanup_browser()
            self.assertFalse(case.profile.exists())


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        raise SystemExit("usage: test_event_loop_chromium_e2e.py DRIVER [SESSION_BRIDGE]")
    gtk.DRIVER = pathlib.Path(sys.argv[1]).resolve()
    if not gtk.DRIVER.is_file():
        raise SystemExit(f"missing EventLoop e2e driver: {gtk.DRIVER}")
    gtk.SESSION_BRIDGE = pathlib.Path(sys.argv[2]).resolve() if len(sys.argv) == 3 else None
    if gtk.SESSION_BRIDGE is not None and not gtk.SESSION_BRIDGE.is_file():
        raise SystemExit(f"missing session bridge: {gtk.SESSION_BRIDGE}")
    if browser_path() is None:
        required = os.environ.get("PUNTO_REQUIRE_EVENT_LOOP_E2E") == "1"
        print(("FAIL" if required else "SKIP") + ": missing native Chromium", file=sys.stderr)
        raise SystemExit(1 if required else gtk.SKIP_EXIT)
    if os.environ.get("PUNTO_EVENT_LOOP_E2E_INNER") != "1":
        gtk.__file__ = __file__
        raise SystemExit(gtk.run_in_sandbox(gtk.DRIVER, gtk.SESSION_BRIDGE))
    gtk.require_private_network_namespace()
    names = sorted(name for name in ChromiumE2E.__dict__ if name.startswith("test_"))
    lifecycle_names = sorted(name for name in BrowserLifecycle.__dict__
                             if name.startswith("test_"))
    names += lifecycle_names
    selected = os.environ.get("PUNTO_EVENT_LOOP_E2E_TEST")
    if selected:
        names = selected.split(",")
        if len(names) != len(set(names)):
            raise SystemExit("duplicate PUNTO_EVENT_LOOP_E2E_TEST selectors")
    else:
        names.remove("test_hundred_manual_conversions_survive_failure_cycles")
        names.remove("test_twenty_six_initial_failures_recover_from_zero_dispatches")
        shard = os.environ.get("PUNTO_EVENT_LOOP_E2E_SHARD")
        if shard:
            index, count = (int(part) for part in shard.split("/", 1))
            if count < 1 or index < 0 or index >= count:
                raise SystemExit(f"invalid PUNTO_EVENT_LOOP_E2E_SHARD={shard}")
            names = [name for position, name in enumerate(names)
                     if position % count == index]
    cases = [
        (BrowserLifecycle if name in lifecycle_names else ChromiumE2E)(name)
        for name in names
    ]
    cases.sort(key=lambda case: type(case).__name__)
    suite = unittest.TestSuite(cases)
    raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
