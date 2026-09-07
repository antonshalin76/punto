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

    def test_repeated_manual_conversion_with_retained_primary(self):
        self.first_manual_conversion()
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "second native browser correction with retained PRIMARY")
        self.pump_until(lambda: self.dispatches() == 2, "second dispatch receipt")

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
        self.layout_shortcut.stop()
        self.layout_shortcut = gtk.DesktopLayoutShortcut(
            self.x11.display,
            modifier=gtk.KEY_LEFTALT,
            key=gtk.KEY_BACKSLASH,
            modifier_mask=8,
        )
        self.layout_shortcut.start()
        self.addCleanup(self.layout_shortcut.stop)

        config = pathlib.Path("/tmp/punto-home/.config/punto/config.yaml")
        original = config.read_text(encoding="utf-8")
        self.addCleanup(config.write_text, original, encoding="utf-8")
        config.write_text(
            original.replace("modifier: leftctrl", "modifier: leftalt")
                    .replace("key: grave", "key: backslash"),
            encoding="utf-8",
        )
        generation = int(self.stats_fields()[1]["config_generation"])
        self.assertEqual(gtk.ipc_request(b"RELOAD\n"), b"OK Scheduled\n")
        self.pump_until(
            lambda: int(self.stats_fields()[1]["config_generation"]) > generation
            and self.stats_fields()[1]["config_result"] == "ok",
            "custom layout shortcut config commit",
        )

        self.first_manual_conversion()
        self.assertEqual(self.layout_shortcut.activations, 1)
        self.assertEqual(self.layout_shortcut.source_index, 1)
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

    def test_layout_shortcut_past_macro_deadline_does_not_mutate_text(self):
        self.layout_shortcut.handling_delay = 0.35
        self.harness.type_word("ghbdtn")
        self.pump_until(lambda: self.browser_state()[:3] == ["ghbdtn", 6, 6],
                        "browser typed source")
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            "late desktop layout shortcut rejection",
            timeout=1,
        )
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after late shortcut rejection")
        self.assertEqual(self.browser_state()[:3], ["ghbdtn", 6, 6])
        self.assertEqual(self.dispatches(), 0)
        self.pump_until(lambda: self.layout_shortcut.activations == 1,
                        "late desktop layout activation")
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after late layout activation")
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
        self.assertEqual(self.keyboard_group(), 0)
        self.assertEqual(self.dispatches(), 0)

    def test_physical_layout_shortcut_preserves_next_correction(self):
        self.first_manual_conversion()

        activations = self.layout_shortcut.activations
        self.send_chord((gtk.KEY_LEFTCTRL,), key=gtk.KEY_GRAVE)
        self.pump_until(
            lambda: self.layout_shortcut.activations == activations + 1
            and self.keyboard_group() == 0,
            "physical desktop layout shortcut",
        )
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
        self.send_chord((gtk.KEY_LEFTCTRL,), key=gtk.KEY_GRAVE)
        self.pump_until(lambda: self.keyboard_group() == 0,
                        "configured layout shortcut after repeat")
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

    def assert_transport_fault_invalidates_retained_receipt(self, marker_name):
        self.first_manual_conversion()
        marker = pathlib.Path("/run", marker_name)
        marker.unlink(missing_ok=True)
        self.addCleanup(marker.unlink, missing_ok=True)
        marker.touch(mode=0o600)
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            f"{marker_name} rejects retained correction",
        )
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, f"fresh DOM after {marker_name}")
        self.assertEqual(self.dispatches(), 1)
        self.assertFalse(marker.exists(), f"{marker_name} fault was not reached")
        self.assert_pause_rejected()
        self.assertEqual(self.dispatches(), 1)

    def test_keymap_failure_invalidates_retained_receipt(self):
        self.assert_transport_fault_invalidates_retained_receipt(
            "punto-e2e-fail-word-keymap")

    def test_layout_hotkey_send_failure_invalidates_retained_receipt(self):
        self.assert_transport_fault_invalidates_retained_receipt(
            "punto-e2e-fail-layout-hotkey-send")

    def assert_context_change_invalidates_retained_receipt(self, change, restore):
        self.first_manual_conversion()
        activations = self.layout_shortcut.activations
        self.layout_shortcut.arm_blocked_activation()
        self.addCleanup(self.layout_shortcut.permit_activation.set)
        before = self.browser_state()
        rejected = self.harness.diagnostic().count("Word edit dispatch status=0")
        self.harness.send_key(gtk.KEY_PAUSE)
        self.pump_until(self.layout_shortcut.activation_started.is_set,
                        "retained correction entered layout preflight")
        change()
        self.pump_until(
            lambda: self.harness.diagnostic().count(
                "Word edit dispatch status=0") > rejected,
            "context change rejects retained correction",
        )
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after context rejection")
        self.assertEqual(self.browser_state()[:6], before[:6])
        self.assertEqual(self.dispatches(), 1)

        self.layout_shortcut.permit_activation.set()
        self.pump_until(
            lambda: self.layout_shortcut.activations == activations + 1
            and self.keyboard_group() == 0,
            "blocked desktop activation completed",
        )
        restore()
        self.assert_pause_rejected()

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

    def test_selection_layout_stall_preserves_browser_selection(self):
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
            "browser selection layout preflight rejection",
        )
        serial = self.browser_state()[-1]
        self.wait_for_fresh_dom(serial, "fresh DOM after selection rejection")
        self.assertEqual(self.browser_state()[3:7], ["ghbdtn", 0, 6, "b"])
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_CLIPBOARD),
                         "startup clipboard baseline")
        self.assertEqual(self.dispatches(), 0)

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
