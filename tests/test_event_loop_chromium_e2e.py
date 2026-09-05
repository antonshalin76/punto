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

    def test_delayed_clipboard_initialization_uses_remaining_macro_budget(self):
        marker = pathlib.Path("/run/punto-e2e-slow-clipboard-init")
        marker.touch(mode=0o600)
        self.first_manual_conversion()
        self.assertFalse(marker.exists(), "clipboard initialization fault was not reached")

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
