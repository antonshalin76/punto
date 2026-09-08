#!/usr/bin/env python3
"""Native Chromium retained-PRIMARY regressions in the private GTK sandbox."""

import json
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

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


class ChromiumE2E(gtk.EventLoopGtkE2E):
    def setUp(self):
        super().setUp()
        self.prepare_word_editor()
        self.assertEqual(gtk.ipc_request(b"SET_STATUS 0\n"), b"OK DISABLED\n")
        self.profile = pathlib.Path(tempfile.mkdtemp(prefix="punto-chromium-"))
        self.browser = None
        self.addCleanup(self.cleanup_browser)
        page = self.profile / "page.html"
        page.write_text(
            '<!doctype html><meta charset="utf-8"><title>starting</title>'
            '<style>input{display:block;margin:20px;width:400px;height:40px;'
            'font-size:24px}</style><input id="a" autofocus>'
            '<input id="b" value="ghbdtn"><script>let keys=0,snapshots=0;'
            'addEventListener("keydown",()=>keys++);'
            'setInterval(()=>document.title="PUNTO"+JSON.stringify('
            '[a.value,a.selectionStart,a.selectionEnd,b.value,b.selectionStart,'
            'b.selectionEnd,document.activeElement.id,keys,'
            '[b.getBoundingClientRect().left,b.getBoundingClientRect().top,'
            'outerHeight-innerHeight],++snapshots]),20)</script>',
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
                ["xdotool", "search", "--name", r"^PUNTO\["],
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
        self.assertTrue(title.startswith("PUNTO["), title)
        return json.loads(title[5:])

    def wait_for_fresh_dom(self, serial, description):
        self.pump_until(lambda: self.browser_state()[-1] > serial, description)

    def dispatches(self):
        return int(self.stats_fields()[1]["word_dispatches"])

    def clipboard_owner(self):
        x11 = gtk.ctypes.CDLL(gtk.ctypes.util.find_library("X11"))
        x11.XOpenDisplay.argtypes = [gtk.ctypes.c_char_p]
        x11.XOpenDisplay.restype = gtk.ctypes.c_void_p
        x11.XInternAtom.argtypes = [gtk.ctypes.c_void_p, gtk.ctypes.c_char_p,
                                    gtk.ctypes.c_int]
        x11.XInternAtom.restype = gtk.ctypes.c_ulong
        x11.XGetSelectionOwner.argtypes = [gtk.ctypes.c_void_p,
                                           gtk.ctypes.c_ulong]
        x11.XGetSelectionOwner.restype = gtk.ctypes.c_ulong
        x11.XCloseDisplay.argtypes = [gtk.ctypes.c_void_p]
        connection = x11.XOpenDisplay(self.x11.display.encode("ascii"))
        self.assertTrue(connection)
        try:
            clipboard = x11.XInternAtom(connection, b"CLIPBOARD", 0)
            return x11.XGetSelectionOwner(connection, clipboard)
        finally:
            x11.XCloseDisplay(connection)

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

    def key_is_down(self, code):
        x11 = gtk.ctypes.CDLL(gtk.ctypes.util.find_library("X11"))
        x11.XOpenDisplay.argtypes = [gtk.ctypes.c_char_p]
        x11.XOpenDisplay.restype = gtk.ctypes.c_void_p
        x11.XQueryKeymap.argtypes = [gtk.ctypes.c_void_p, gtk.ctypes.c_void_p]
        x11.XCloseDisplay.argtypes = [gtk.ctypes.c_void_p]
        connection = x11.XOpenDisplay(self.x11.display.encode("ascii"))
        self.assertTrue(connection, "key observation opens only the private Xvfb")
        try:
            keys = (gtk.ctypes.c_ubyte * 32)()
            self.assertNotEqual(x11.XQueryKeymap(connection, keys), 0)
            keycode = code + 8
            return bool(keys[keycode // 8] & (1 << (keycode % 8)))
        finally:
            x11.XCloseDisplay(connection)

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

    def assert_pause_rejected(self):
        before = self.browser_state()
        dispatches = self.dispatches()
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count("Word edit dispatch status=0") > rejected,
            "browser rejection before mutation",
        )
        serial = self.browser_state()[-1]
        self.pump_until(lambda: self.browser_state()[-1] > serial,
                        "fresh DOM snapshot after rejection")
        self.assertEqual(self.browser_state()[:-1], before[:-1])
        self.assertEqual(self.dispatches(), dispatches)

    def assert_stale_primary_blocks_fresh_word(self):
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
        self.assert_pause_rejected()

    def test_repeated_manual_conversion_with_retained_primary(self):
        self.first_manual_conversion()
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "second native browser correction with retained PRIMARY")
        self.pump_until(lambda: self.dispatches() == 2, "second dispatch receipt")

    def test_word_layout_selection_disturbance_never_inserts_at_caret(self):
        self.harness.type_word("ghbdtn")
        self.pump_until(
            lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
            "browser word before desktop selection disturbance",
        )
        before = self.browser_state()
        dispatches = self.dispatches()
        rejected = self.harness.diagnostic().count(
            "rejection_stage=layout_transition_foreign_raw_key"
        )
        self.layout_shortcut.selection_disturbance = True
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.layout_shortcut.selection_disturbances == 1,
            "browser word selection disturbed by desktop transition",
        )
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "rejection_stage=layout_transition_foreign_raw_key"
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

    def test_four_manual_conversions_retain_each_successful_receipt(self):
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
                self.assertEqual(self.layout_shortcut.source_index, group)

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
        self.assertEqual(self.layout_shortcut.source_index, 1)
        self.assertFalse(self.key_is_down(gtk.KEY_LEFTCTRL))
        self.assertFalse(self.key_is_down(gtk.KEY_GRAVE))

    def test_correction_uses_reloaded_layout_shortcut(self):
        self.reload_layout_shortcut(
            gtk.KEY_LEFTALT, gtk.KEY_BACKSLASH, 8,
            "leftctrl", "leftalt", "grave", "backslash",
        )

        self.first_manual_conversion()
        self.assertEqual(self.layout_shortcut.activations, 1)
        self.assertEqual(self.layout_shortcut.source_index, 1)
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
        self.assert_transport_fault_invalidates_retained_receipt(
            "punto-e2e-fail-layout-hotkey-send", prepared=True)
        self.assertEqual(self.layout_shortcut.activations, 2)
        self.assertEqual(self.layout_shortcut.source_index, 0)
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
        self.assertEqual(self.layout_shortcut.source_index, 1)
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
        self.assertEqual(self.layout_shortcut.source_index, 1)
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
                self.assertEqual(self.layout_shortcut.source_index, 1)
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

    def test_own_layout_chord_transient_state_settles_before_correction(self):
        marker = pathlib.Path(
            "/run/punto-e2e-inject-own-layout-modifier-state")
        self.addCleanup(marker.unlink, missing_ok=True)
        marker.touch(mode=0o600)
        self.layout_shortcut.handling_delay = 0.05
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "browser typed before transient layout state")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == ["привет", 6, 6],
                        "correction after own layout chord settled")
        self.assertFalse(marker.exists())
        self.assertEqual(self.dispatches(), 1)
        self.assertEqual(self.keyboard_group(), 1)

    def test_layout_shortcut_focus_transition_settles_before_correction(self):
        gtk_window = self.window.get_window().get_xid()
        self.layout_shortcut.focus_transition = (gtk_window,
                                                 self.browser_window)
        self.layout_shortcut.handling_delay = 0.05
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "browser typed before desktop focus transition")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == ["привет", 6, 6],
                        "correction after desktop focus transition")
        self.assertEqual(self.layout_shortcut.focus_transitions, 1)
        self.assertEqual(self.dispatches(), 1)
        self.assertEqual(self.keyboard_group(), 1)

    def test_layout_transition_quiesces_before_text_mutation(self):
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
        self.first_manual_conversion()
        self.assertFalse(markers["arm-layout-settle-window"].exists())
        self.assertFalse(markers["layout-settle-too-early"].exists())
        self.assertTrue(markers["layout-settle-observed"].exists())

    def test_foreign_modifier_during_layout_transition_rejects_correction(self):
        marker = pathlib.Path(
            "/run/punto-e2e-inject-foreign-layout-modifier-state")
        self.addCleanup(marker.unlink, missing_ok=True)
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
        self.assertEqual(self.browser_state()[:3], ["ghbdtn", 6, 6])
        self.assertEqual(self.dispatches(), 0)

    def test_foreign_key_during_own_layout_transition_rejects_correction(self):
        markers = [
            pathlib.Path("/run/punto-e2e-inject-own-layout-modifier-state"),
            pathlib.Path(
                "/run/punto-e2e-inject-foreign-key-after-layout-state"),
        ]
        for marker in markers:
            self.addCleanup(marker.unlink, missing_ok=True)
            marker.touch(mode=0o600)
        self.layout_shortcut.handling_delay = 0.05
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "browser typed before foreign key state")
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            "foreign key layout rejection",
        )
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after foreign key rejection")
        self.assertTrue(all(not marker.exists() for marker in markers))
        self.assertIn("rejection_stage=layout_transition_foreign_key",
                      self.harness.diagnostic())
        self.assertEqual(self.browser_state()[:3], ["ghbdtn", 6, 6])
        self.assertEqual(self.dispatches(), 0)

    def test_layout_transition_past_extended_budget_does_not_mutate_text(self):
        self.layout_shortcut.handling_delay = 1.1
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "browser typed source")
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            "desktop layout transition budget rejection",
            timeout=3,
        )
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after layout timeout")
        self.assertEqual(self.browser_state()[:3], ["ghbdtn", 6, 6])
        self.assertEqual(self.dispatches(), 0)
        self.pump_until(lambda: self.layout_shortcut.activations == 1,
                        "late desktop layout activation")
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after late activation")
        self.assertEqual(self.browser_state()[:3], ["ghbdtn", 6, 6])
        self.assertFalse(self.key_is_down(gtk.KEY_LEFTCTRL))
        self.assertFalse(self.key_is_down(gtk.KEY_GRAVE))

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
        self.assertEqual(self.layout_shortcut.source_index, 1)
        self.harness.type_word("f")
        self.pump_until(
            lambda: self.browser_state()[:3] == ["привет привета", 14, 14],
            "next physical character uses corrected Russian layout",
        )

    def test_wrong_modifier_side_invalidates_retained_receipt(self):
        self.first_manual_conversion()
        activations = self.layout_shortcut.activations
        self.send_chord((gtk.KEY_RIGHTCTRL,), key=gtk.KEY_GRAVE)
        self.pump_until(
            lambda: self.layout_shortcut.activations == activations + 1
            and self.keyboard_group() == 0,
            "desktop shortcut from unconfigured Control side",
        )
        self.harness.send_key(gtk.KEY_SPACE)
        self.harness.type_word("ghbdtn")
        self.pump_until(
            lambda: self.browser_state()[:3] == ["привет ghbdtn", 13, 13],
            "fresh source after unconfigured Control side",
        )
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            "retained receipt invalidation for wrong modifier side",
        )
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after wrong-side rejection")
        self.assertEqual(self.browser_state()[:3], ["привет ghbdtn", 13, 13])
        self.assertEqual(self.dispatches(), 1)

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
        self.harness.send_key(gtk.KEY_SPACE)
        self.harness.type_word("ghbdtn")
        self.pump_until(
            lambda: self.browser_state()[:3] == ["привет ghbdtn", 13, 13],
            "fresh source after extra-modifier chord",
        )
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            "retained receipt invalidation for extra modifier",
        )
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after extra-modifier rejection")
        self.assertEqual(self.browser_state()[:3], ["привет ghbdtn", 13, 13])
        self.assertEqual(self.dispatches(), 1)

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
        self.layout_shortcut.enabled = False
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            "safe repeated correction rejection",
        )
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after safe repeated rejection")
        self.assertEqual(self.browser_state()[:3], ["привет", 6, 6])
        self.assertEqual(self.dispatches(), 1)

        self.layout_shortcut.enabled = True
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "retained receipt recovery without restart")
        self.assertEqual(self.dispatches(), 2)

    def assert_transport_fault_invalidates_retained_receipt(
            self, marker_name, *, prepared=False):
        self.first_manual_conversion()
        marker = pathlib.Path("/run", marker_name)
        marker.unlink(missing_ok=True)
        self.addCleanup(marker.unlink, missing_ok=True)
        marker.touch(mode=0o600)
        status = 1 if prepared else 0
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
        if prepared:
            self.assert_stale_primary_blocks_fresh_word()
        else:
            self.assert_pause_rejected()
        self.assertEqual(self.dispatches(), 1)

    def test_keymap_failure_invalidates_retained_receipt(self):
        self.assert_transport_fault_invalidates_retained_receipt(
            "punto-e2e-fail-word-keymap")

    def test_layout_hotkey_send_failure_invalidates_retained_receipt(self):
        self.assert_transport_fault_invalidates_retained_receipt(
            "punto-e2e-fail-layout-hotkey-send", prepared=True)

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
        self.assert_stale_primary_blocks_fresh_word()

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

    def test_selection_layout_stall_reports_partial_after_browser_replay(self):
        left, top, decoration = self.browser_state()[8]
        self.xdo("mousemove", "--window", str(self.browser_window),
                 str(int(left + 30)), str(int(top + decoration + 20)))
        self.xdo("click", "--repeat", "2", "--delay", "100", "1")
        self.pump_until(lambda: self.browser_state()[3:7] ==
                        ["ghbdtn", 0, 6, "b"],
                        "browser selection before layout preflight")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_PRIMARY), "ghbdtn")
        self.layout_shortcut.enabled = False
        rejected = self.harness.diagnostic().count("Word edit dispatch status=3")
        self.send_chord((gtk.KEY_LEFTSHIFT,))
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=3") > rejected,
            "browser final layout transition failure",
        )
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after partial selection replay")
        self.assertEqual(self.browser_state()[3:7], ["привет", 6, 6, "b"])
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.assertEqual(self.dispatches(), 0)

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
        self.assertEqual(self.layout_shortcut.source_index, 1)
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
        self.assertEqual(self.layout_shortcut.source_index, 1)
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
                        "browser selection before internal-layout fault")
        self.xdo("key", "ctrl+c")
        self.pump_until(
            lambda: self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD) == source,
            "rich clipboard before internal-layout fault",
        )
        return self.rich_clipboard_snapshot()

    def assert_internal_layout_failure_restores_source(self, marker_name,
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
        self.assertEqual(self.layout_shortcut.source_index, 0)
        self.assertEqual(self.keyboard_group(), 0, self.harness.diagnostic())
        self.assertEqual(self.rich_clipboard_snapshot(), clipboard_before)
        left, top, decoration = self.browser_state()[8]
        self.xdo("mousemove", "--window", str(self.browser_window),
                 str(int(left + 250)), str(int(top + decoration + 20)))
        self.xdo("click", "1")
        self.pump_until(lambda: self.browser_state()[6] == "b",
                        "browser target refocused after internal-layout fault")
        self.harness.type_word("a")
        self.pump_until(lambda: self.browser_state()[6] == "b" and
                        self.browser_state()[3].endswith("a"),
                        "next physical key uses restored English group")

    def test_internal_layout_failure_before_first_replay_restores_source(self):
        self.assert_internal_layout_failure_restores_source(
            "punto-e2e-fail-after-internal-layout")

    def test_internal_layout_failure_after_first_replay_restores_source(self):
        self.assert_internal_layout_failure_restores_source(
            "punto-e2e-fail-after-first-replay-stroke")

    def test_internal_layout_deadline_restores_source(self):
        self.assert_internal_layout_failure_restores_source(
            "punto-e2e-expire-after-internal-layout", status=0)

    def test_internal_layout_cancellation_restores_source(self):
        clipboard_before = self.prepare_rich_browser_selection()
        markers = {
            name: pathlib.Path("/run", "punto-e2e-" + name)
            for name in ("arm-after-internal-layout", "after-internal-layout",
                         "release-after-internal-layout")
        }
        for marker in markers.values():
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        self.addCleanup(markers["release-after-internal-layout"].touch,
                        exist_ok=True)
        markers["arm-after-internal-layout"].touch(mode=0o600)
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.send_chord((gtk.KEY_LEFTSHIFT,))
        self.pump_until(markers["after-internal-layout"].exists,
                        "internal layout reached before cancellation")
        with gtk.socket.socket(gtk.socket.AF_UNIX, gtk.socket.SOCK_STREAM) as client:
            client.settimeout(gtk.EVENT_TIMEOUT)
            client.connect("/run/punto.sock")
            client.sendall(b"SET_STATUS 0\n")
            self.pump_for(0.05)
            markers["release-after-internal-layout"].touch(mode=0o600)
            self.assertEqual(client.recv(8192), b"OK DISABLED\n")
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            "queued mutation cancelled internal replay",
        )
        self.assertEqual(self.layout_shortcut.source_index, 0)
        self.assertEqual(self.keyboard_group(), 0, self.harness.diagnostic())
        self.assertEqual(self.rich_clipboard_snapshot(), clipboard_before)

    def assert_nonlayout_internal_failure_restores_source(
            self, source_group, modifiers):
        clipboard_before = self.prepare_rich_browser_selection(
            source="привет", source_group=source_group)
        marker = pathlib.Path("/run/punto-e2e-fail-after-internal-layout")
        marker.unlink(missing_ok=True)
        marker.touch(mode=0o600)
        self.addCleanup(marker.unlink, missing_ok=True)
        partial = self.harness.diagnostic().count("Word edit dispatch status=3")
        self.send_chord(modifiers)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=3") > partial,
            "non-layout rich replay fault observed",
        )
        self.assertEqual(self.layout_shortcut.source_index, source_group)
        self.assertEqual(self.keyboard_group(), source_group,
                         self.harness.diagnostic())
        self.assertEqual(self.rich_clipboard_snapshot(), clipboard_before)

    def test_selection_case_internal_failure_restores_source(self):
        self.assert_nonlayout_internal_failure_restores_source(
            0, (gtk.KEY_LEFTALT,))

    def test_selection_translit_internal_failure_restores_source(self):
        self.assert_nonlayout_internal_failure_restores_source(
            1, (gtk.KEY_LEFTCTRL, gtk.KEY_LEFTALT))

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

    def test_new_same_text_selection_is_not_a_retained_receipt(self):
        self.first_manual_conversion()
        left, top, decoration = self.browser_state()[8]
        self.xdo("mousemove", "--window", str(self.browser_window),
                 str(int(left + 30)), str(int(top + decoration + 20)))
        self.xdo("click", "--repeat", "2", "--delay", "100", "1")
        self.pump_until(lambda: self.browser_state()[3:7] == ["ghbdtn", 0, 6, "b"],
                        "genuine same-text browser selection")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_PRIMARY), "ghbdtn")
        self.assert_pause_rejected()

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

    def test_runtime_reset_invalidates_retained_receipt(self):
        self.first_manual_conversion()
        self.assertEqual(gtk.ipc_request(b"SET_STATUS 0\n"), b"OK DISABLED\n")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_PRIMARY), "ghbdtn")
        self.assert_pause_rejected()


class BrowserLifecycle(unittest.TestCase):
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
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_event_loop_chromium_e2e.py DRIVER")
    gtk.DRIVER = pathlib.Path(sys.argv[1]).resolve()
    if not gtk.DRIVER.is_file():
        raise SystemExit(f"missing EventLoop e2e driver: {gtk.DRIVER}")
    if browser_path() is None:
        required = os.environ.get("PUNTO_REQUIRE_EVENT_LOOP_E2E") == "1"
        print(("FAIL" if required else "SKIP") + ": missing native Chromium", file=sys.stderr)
        raise SystemExit(1 if required else gtk.SKIP_EXIT)
    if os.environ.get("PUNTO_EVENT_LOOP_E2E_INNER") != "1":
        gtk.__file__ = __file__
        raise SystemExit(gtk.run_in_sandbox(gtk.DRIVER))
    gtk.require_private_network_namespace()
    names = sorted(name for name in ChromiumE2E.__dict__ if name.startswith("test_"))
    lifecycle_names = sorted(name for name in BrowserLifecycle.__dict__
                             if name.startswith("test_"))
    names += lifecycle_names
    selected = os.environ.get("PUNTO_EVENT_LOOP_E2E_TEST")
    if selected:
        names = selected.split(",")
    suite = unittest.TestSuite(
        (BrowserLifecycle if name in lifecycle_names else ChromiumE2E)(name)
        for name in names
    )
    raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
