#!/usr/bin/env python3
"""Native Chromium retained-PRIMARY regressions in the private GTK sandbox."""

import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

import test_event_loop_gtk_e2e as gtk


def browser_path():
    return next((path for name in ("google-chrome", "chromium", "chromium-browser")
                 if (path := shutil.which(name))), None)


class ChromiumE2E(gtk.EventLoopGtkE2E):
    def setUp(self):
        super().setUp()
        self.prepare_word_editor()
        self.assertEqual(gtk.ipc_request(b"SET_STATUS 0\n"), b"OK DISABLED\n")
        self.profile = tempfile.TemporaryDirectory(prefix="punto-chromium-")
        self.addCleanup(self.profile.cleanup)
        page = pathlib.Path(self.profile.name, "page.html")
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
             f"--user-data-dir={self.profile.name}/profile", f"--app={page.as_uri()}"],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=self.browser_log,
        )
        self.addCleanup(self.stop_browser)
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
        if self.browser.poll() is None:
            self.browser.terminate()
            try:
                self.browser.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.browser.kill()
                self.browser.wait(timeout=3)

    def browser_state(self):
        title = self.xdo("getwindowname", str(self.browser_window))
        self.assertTrue(title.startswith("PUNTO["), title)
        return json.loads(title[5:])

    def dispatches(self):
        return int(self.stats_fields()[1]["word_dispatches"])

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

    def test_consecutive_automatic_corrections(self):
        self.assertEqual(gtk.ipc_request(b"SET_STATUS 1\n"), b"OK ENABLED\n")
        self.harness.type_word("ghbdtn")
        self.harness.send_key(gtk.KEY_SPACE)
        self.pump_until(lambda: self.browser_state()[:3] == ["привет ", 7, 7],
                        "first browser automatic correction")
        self.harness.type_word("hello")
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

    def test_runtime_reset_invalidates_retained_receipt(self):
        self.first_manual_conversion()
        self.assertEqual(gtk.ipc_request(b"SET_STATUS 0\n"), b"OK DISABLED\n")
        self.assertEqual(self.selection_text(gtk.Gdk.SELECTION_PRIMARY), "ghbdtn")
        self.assert_pause_rejected()


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
    selected = os.environ.get("PUNTO_EVENT_LOOP_E2E_TEST")
    if selected:
        names = selected.split(",")
    suite = unittest.TestSuite(ChromiumE2E(name) for name in names)
    raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
