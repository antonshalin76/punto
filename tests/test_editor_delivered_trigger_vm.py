#!/usr/bin/env python3
"""Private G1-P input-stream tests; never use this relay for installed Punto."""

import ctypes
import json
import os
import pathlib
import subprocess
import sys
import threading
import time
import unittest

import test_event_loop_gtk_e2e as base


EV_MSC = 4
MSC_SERIAL = 0
PRIVATE_PAUSE_WORD = 0x50550001
BOUNDARY_ATOM = "_PUNTO_PRIVATE_DELIVERED_TRIGGER"


class DeliveredRelay(base.XTestRelay):
    def __init__(self, stream, display: str) -> None:
        super().__init__(stream, display)
        self.target_window = 0
        self.press_count = 0
        self.pending_boundary: int | None = None
        self.boundary_ack = threading.Event()
        self.private_marker_seen = threading.Event()
        self.expired_boundaries: list[int] = []
        self.expected_word = ""
        self.target_pid = 0
        self.probe_path: pathlib.Path | None = None
        self.probe_result: tuple[int, str, str] | None = None
        self.probe_results: list[tuple[int, str, str]] = []
        self.duplicate_once = False

    def handle_key_delivered(self, code: int, value: int) -> None:
        if value == 1:
            self.press_count += 1

    def handle_non_key(self, event_type: int, code: int, value: int,
                       connection: int, x11) -> None:
        if (event_type, code, value) != (EV_MSC, MSC_SERIAL,
                                           PRIVATE_PAUSE_WORD):
            return
        if not self.target_window:
            return
        self.private_marker_seen.set()
        x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                   ctypes.c_int]
        x11.XInternAtom.restype = ctypes.c_ulong
        x11.XChangeProperty.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong,
            ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
            ctypes.c_void_p, ctypes.c_int,
        ]
        serial = ctypes.c_ulong(self.press_count)
        atom = x11.XInternAtom(connection, BOUNDARY_ATOM.encode("ascii"), 0)
        cardinal = x11.XInternAtom(connection, b"CARDINAL", 0)
        self.boundary_ack.clear()
        self.pending_boundary = self.press_count
        x11.XChangeProperty(connection, self.target_window, atom, cardinal,
                            32, 0, ctypes.byref(serial), 1)
        x11.XFlush(connection)
        if not self.boundary_ack.wait(0.5):
            self.expired_boundaries.append(self.press_count)
            self.pending_boundary = None
            return
        self.pending_boundary = None
        if not self.expected_word or self.probe_path is None:
            return
        try:
            result = subprocess.run(
                [str(self.probe_path), str(self.target_pid),
                 str(self.target_window), self.expected_word, "400"],
                capture_output=True, text=True, timeout=0.5, check=False)
            self.probe_result = (result.returncode, result.stdout,
                                 result.stderr)
        except subprocess.TimeoutExpired:
            self.probe_result = (4, "", "UNKNOWN probe_timeout")
        self.probe_results.append(self.probe_result)
        if self.duplicate_once:
            self.duplicate_once = False
            self.handle_non_key(event_type, code, value, connection, x11)


class DeliveredTriggerTests(base.EventLoopGtkE2E):
    relay_class = DeliveredRelay

    def setUp(self) -> None:
        super().setUp()
        self.processed_key_presses = 0
        self.boundary_text: list[tuple[int, str]] = []
        self.boundary_preedit: list[bool] = []
        self.preedit_by_widget: dict[object, bool] = {}
        self.pending_ordinal: int | None = None
        self.pending_widget = None
        self.disturb_before_ack = False
        self.entry.connect("event-after", self._count_key_press)
        self.entry.connect("preedit-changed", self._on_preedit)
        gdk_window = self.window.get_window()
        gdk_window.set_events(gdk_window.get_events() |
                              base.Gdk.EventMask.PROPERTY_CHANGE_MASK)
        self.window.connect("property-notify-event", self._on_property)
        self.harness.relay.target_window = gdk_window.get_xid()
        self.harness.relay.target_pid = os.getpid()
        self.harness.relay.probe_path = pathlib.Path(
            os.environ["PUNTO_DELIVERED_PROBE"])

    def _count_key_press(self, _widget, event) -> None:
        if event.type == base.Gdk.EventType.KEY_PRESS:
            self.processed_key_presses += 1
            if self.pending_ordinal is not None:
                base.GLib.idle_add(self._finish_boundary)

    def _visible_text(self, widget=None) -> str:
        widget = widget or self.entry
        if isinstance(widget, base.Gtk.Entry):
            return widget.get_text()
        buffer = widget.get_buffer()
        return buffer.get_text(buffer.get_start_iter(), buffer.get_end_iter(),
                               True)

    def _on_preedit(self, widget, preedit: str) -> None:
        self.preedit_by_widget[widget] = bool(preedit)
        if (not preedit and widget is self.pending_widget and
                self.pending_ordinal is not None):
            base.GLib.idle_add(self._finish_boundary)

    def _finish_boundary(self) -> bool:
        ordinal = self.pending_ordinal
        widget = self.pending_widget
        if (ordinal is None or widget is None or
                self.preedit_by_widget.get(widget, False) or
                self.harness.relay.pending_boundary != ordinal):
            return False
        if (not widget.has_focus() or
                self.window.get_focus() is not widget or
                self.processed_key_presses != ordinal):
            return False
        word = self._visible_text(widget)
        if not word:
            return False
        self.boundary_text.append((ordinal, word))
        self.harness.relay.expected_word = word
        if self.disturb_before_ack:
            self.disturb_before_ack = False
            widget.set_text("foreign")
        self.pending_ordinal = None
        self.pending_widget = None
        self.harness.relay.boundary_ack.set()
        return False

    def _on_property(self, _widget, event) -> bool:
        if event.atom.name() == BOUNDARY_ATOM:
            ordinal = self.harness.relay.pending_boundary
            if ordinal is not None:
                self.pending_ordinal = ordinal
                self.pending_widget = self.window.get_focus()
                self.boundary_preedit.append(
                    self.preedit_by_widget.get(self.pending_widget, False))
                base.GLib.idle_add(self._finish_boundary)
        return False

    def test_pause_lifecycle_emits_one_private_control(self) -> None:
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "hello",
                        "ordinary prefix delivered to GTK")
        before = len(self.harness.relay.snapshot())
        self.harness.send_events([
            (base.EV_KEY, base.KEY_PAUSE, 1),
            (base.EV_SYN, base.SYN_REPORT, 0),
            (base.EV_KEY, base.KEY_PAUSE, 2),
            (base.EV_SYN, base.SYN_REPORT, 0),
            (base.EV_KEY, base.KEY_PAUSE, 0),
            (base.EV_SYN, base.SYN_REPORT, 0),
        ])
        self.pump_until(
            lambda: any(event[1:] == (EV_MSC, MSC_SERIAL,
                                      PRIVATE_PAUSE_WORD)
                        for event in self.harness.relay.snapshot()[before:]),
            "one private Pause control after the input prefix",
        )
        self.wait_event_loop_idle("Pause lifecycle")
        emitted = self.harness.relay.snapshot()[before:]
        self.assertEqual(sum(event[1:] == (EV_MSC, MSC_SERIAL,
                                           PRIVATE_PAUSE_WORD)
                             for event in emitted), 1)
        self.assertFalse(any(event[1] == base.EV_KEY and
                             event[2] == base.KEY_PAUSE for event in emitted))
        self.pump_until(lambda: bool(self.boundary_text),
                        "server-delivered boundary at GTK")
        self.assertEqual(self.boundary_text, [(5, "hello")])
        self.assertEqual(self.processed_key_presses, 5)
        self.assertEqual(self.harness.relay.expired_boundaries, [])
        self.pump_until(lambda: self.harness.relay.probe_result is not None,
                        "quiescent editor-owned correction")
        self.assertEqual(self.harness.relay.probe_result[0], 0,
                         self.harness.relay.probe_result)
        self.assertEqual(self.entry.get_text(), "руддщ")

    def test_delayed_im_commit_settles_before_probe(self) -> None:
        self.harness.type_word("hello")
        self.harness.send_key(base.KEY_PAUSE)
        self.pump_until(lambda: bool(self.boundary_text),
                        "boundary after controlled delayed IM commit")
        self.assertEqual(self.boundary_preedit, [True])
        self.assertEqual(self.boundary_text, [(5, "hello")])
        self.pump_until(lambda: self.entry.get_text() == "hello",
                        "controlled delayed IM commit")
        self.pump_until(lambda: self.harness.relay.probe_result is not None,
                        "correction admitted after IM settlement")
        self.assertEqual(self.harness.relay.probe_result[0], 0,
                         self.harness.relay.probe_result)
        self.assertEqual(self.entry.get_text(), "руддщ")

    def test_textview_correction_uses_same_delivered_boundary(self) -> None:
        self.window.remove(self.entry)
        self.entry = base.Gtk.TextView()
        self.entry.set_wrap_mode(base.Gtk.WrapMode.NONE)
        self.entry.connect("event-after", self._count_key_press)
        self.entry.connect("preedit-changed", self._on_preedit)
        self.window.add(self.entry)
        self.window.show_all()
        self.entry.grab_focus()
        self.pump_until(self.entry.has_focus, "TextView focus")
        self.harness.type_word("hello")
        self.harness.send_key(base.KEY_PAUSE)
        self.pump_until(lambda: self.harness.relay.probe_result is not None,
                        "TextView correction after delivered boundary")
        self.assertEqual(self.boundary_text, [(5, "hello")])
        self.assertEqual(self.harness.relay.probe_result[0], 0,
                         self.harness.relay.probe_result)
        self.assertEqual(self._visible_text(), "руддщ")

    def test_control_registered_before_last_key_handler_finishes(self) -> None:
        last_key_entered = threading.Event()
        release_last_key = threading.Event()
        marker_registered_early = threading.Event()

        def hold_last_key(_widget, event) -> bool:
            if base.Gdk.keyval_name(event.keyval) == "o":
                last_key_entered.set()
                release_last_key.wait(0.3)
            return False

        def release_on_marker() -> None:
            if last_key_entered.wait(1.0):
                if self.harness.relay.private_marker_seen.wait(0.25):
                    marker_registered_early.set()
            release_last_key.set()

        self.entry.connect("key-press-event", hold_last_key)
        thread = threading.Thread(target=release_on_marker, daemon=True)
        thread.start()
        self.harness.type_word("hello")
        self.harness.send_key(base.KEY_PAUSE)
        self.pump_until(lambda: self.harness.relay.probe_result is not None,
                        "last key completes before origin-bound correction")
        thread.join(timeout=1.0)
        self.assertTrue(marker_registered_early.is_set(),
                        "control must register while the last key is pending")
        self.assertEqual(self.boundary_text, [(5, "hello")])
        self.assertEqual(self.harness.relay.probe_result[0], 0,
                         self.harness.relay.probe_result)
        self.assertEqual(self.entry.get_text(), "руддщ")

    def test_same_text_returns_to_original_field_without_retarget(self) -> None:
        first = self.entry
        second = base.Gtk.Entry()
        second.connect("event-after", self._count_key_press)
        second.connect("preedit-changed", self._on_preedit)
        box = base.Gtk.Box(orientation=base.Gtk.Orientation.VERTICAL)
        self.window.remove(first)
        box.pack_start(first, False, False, 0)
        box.pack_start(second, False, False, 0)
        self.window.add(box)
        self.window.show_all()
        for widget, ordinal in ((first, 5), (second, 10), (first, 15)):
            widget.grab_focus()
            self.pump_until(widget.has_focus, "origin field focus")
            widget.set_text("")
            self.entry = widget
            self.harness.relay.probe_result = None
            self.harness.type_word("hello")
            self.harness.send_key(base.KEY_PAUSE)
            self.pump_until(lambda: self.harness.relay.probe_result is not None,
                            "origin-bound correction")
            self.assertEqual(self.harness.relay.probe_result[0], 0,
                             self.harness.relay.probe_result)
            self.assertEqual(widget.get_text(), "руддщ")
            self.assertEqual(self.boundary_text[-1], (ordinal, "hello"))
        self.assertEqual(self.boundary_text,
                         [(5, "hello"), (10, "hello"), (15, "hello")])

    def test_late_im_commit_does_not_revive_expired_boundary(self) -> None:
        self.harness.type_word("hello")
        before = len(self.harness.relay.snapshot())
        self.harness.send_key(base.KEY_PAUSE)
        self.pump_until(lambda: bool(self.harness.relay.expired_boundaries),
                        "private boundary expires before delayed commit")
        self.assertEqual(self.harness.relay.expired_boundaries, [5])
        self.assertEqual(self.boundary_text, [])
        self.pump_until(lambda: self.entry.get_text() == "hello",
                        "late IM commit reaches original editor")
        self.assertIsNone(self.harness.relay.probe_result)
        self.assertFalse(any(event[1] == base.EV_KEY and
                             event[2] == base.KEY_PAUSE
                             for event in self.harness.relay.snapshot()[before:]))
        self.harness.send_key(base.KEY_PAUSE)
        self.pump_until(lambda: self.harness.relay.probe_result is not None,
                        "fresh request after expired boundary")
        self.assertEqual(self.boundary_text, [(5, "hello")])
        self.assertEqual(self.harness.relay.probe_result[0], 0,
                         self.harness.relay.probe_result)
        self.assertEqual(self.entry.get_text(), "руддщ")

    def test_expired_composition_keeps_native_space_and_tab(self) -> None:
        first = self.entry
        second = base.Gtk.Entry()
        box = base.Gtk.Box(orientation=base.Gtk.Orientation.VERTICAL)
        self.window.remove(first)
        box.pack_start(first, False, False, 0)
        box.pack_start(second, False, False, 0)
        self.window.add(box)
        self.window.show_all()
        first.grab_focus()
        self.pump_until(first.has_focus, "composition source focus")
        self.harness.type_word("hello")
        self.harness.send_key(base.KEY_PAUSE)
        self.harness.send_key(base.KEY_SPACE)
        self.pump_until(lambda: self.harness.relay.expired_boundaries == [5],
                        "space follows expired private request")
        self.pump_until(lambda: first.get_text() == "hello ",
                        "IM commit and native Space")
        self.assertIsNone(self.harness.relay.probe_result)
        self.assertEqual(self.output_count(base.KEY_SPACE, 1), 1)
        first.set_text("")
        first.grab_focus()
        self.harness.type_word("hello")
        self.harness.send_key(base.KEY_PAUSE)
        self.harness.send_key(base.KEY_TAB)
        self.pump_until(lambda: self.harness.relay.expired_boundaries == [5, 11],
                        "Tab follows expired private request")
        self.pump_until(second.has_focus, "native Tab traversal after IM")
        self.assertEqual(first.get_text(), "hello")
        self.assertEqual(self.output_count(base.KEY_TAB, 1), 1)
        self.assertEqual(self.output_count(base.KEY_TAB, 0), 1)
        self.assertIsNone(self.harness.relay.probe_result)

    def test_absent_receiver_swallows_pause_and_keeps_next_input(self) -> None:
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "hello",
                        "prefix in GTK without adapter receiver")
        before = len(self.harness.relay.snapshot())
        self.harness.send_key(base.KEY_PAUSE)
        self.pump_until(lambda: self.harness.relay.probe_result is not None,
                        "absent adapter rejection")
        self.assertEqual(self.boundary_text, [(5, "hello")])
        self.assertEqual(self.harness.relay.probe_result[0], 3,
                         self.harness.relay.probe_result)
        self.assertEqual(self.entry.get_text(), "hello")
        self.assertFalse(any(event[1] == base.EV_KEY and
                             event[2] == base.KEY_PAUSE
                             for event in self.harness.relay.snapshot()[before:]))
        self.assertFalse(any(event[1] == "press" and event[2] == "Pause"
                             for event in self.key_events))
        self.harness.type_word("a")
        self.pump_until(lambda: self.entry.get_text() == "helloa",
                        "ordinary input continues after missing receiver")

    def test_selected_installed_im_corrects_entry(self) -> None:
        self.harness.type_word("hello")
        self.harness.send_key(base.KEY_PAUSE)
        self.pump_until(lambda: self.harness.relay.probe_result is not None,
                        "installed IM correction")
        self.assertIn("im-cedilla.so",
                      pathlib.Path("/proc/self/maps").read_text())
        self.assertEqual(self.boundary_text, [(5, "hello")])
        self.assertEqual(self.harness.relay.probe_result[0], 0,
                         self.harness.relay.probe_result)
        self.assertEqual(self.entry.get_text(), "руддщ")

    def test_source_cannot_forge_private_control_record(self) -> None:
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "hello",
                        "prefix before forged source record")
        before = len(self.harness.relay.snapshot())
        self.harness.send_events([
            (EV_MSC, MSC_SERIAL, PRIVATE_PAUSE_WORD),
            (base.EV_SYN, base.SYN_REPORT, 0),
        ])
        self.wait_event_loop_idle("forged source marker")
        self.assertFalse(any(event[1:] == (EV_MSC, MSC_SERIAL,
                                           PRIVATE_PAUSE_WORD)
                             for event in self.harness.relay.snapshot()[before:]))
        self.assertIsNone(self.harness.relay.probe_result)
        self.assertEqual(self.entry.get_text(), "hello")
        self.harness.send_key(base.KEY_PAUSE)
        self.pump_until(lambda: self.harness.relay.probe_result is not None,
                        "legitimate Pause after rejected source marker")
        self.assertEqual(self.harness.relay.probe_result[0], 0,
                         self.harness.relay.probe_result)
        self.assertEqual(self.entry.get_text(), "руддщ")

    def test_stale_origin_rejects_then_fresh_request_recovers(self) -> None:
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "hello",
                        "prefix before stale-origin control")
        self.disturb_before_ack = True
        self.harness.send_key(base.KEY_PAUSE)
        self.pump_until(lambda: self.harness.relay.probe_result is not None,
                        "stale-origin rejection")
        self.assertEqual(self.harness.relay.probe_result[0], 3,
                         self.harness.relay.probe_result)
        self.assertEqual(self.entry.get_text(), "foreign")
        self.entry.set_text("hello")
        self.entry.set_position(5)
        self.harness.relay.probe_result = None
        self.harness.send_key(base.KEY_PAUSE)
        self.pump_until(lambda: self.harness.relay.probe_result is not None,
                        "fresh origin after rejection")
        self.assertEqual(self.harness.relay.probe_result[0], 0,
                         self.harness.relay.probe_result)
        self.assertEqual(self.entry.get_text(), "руддщ")

    def test_duplicate_private_record_never_mutates_twice(self) -> None:
        self.harness.type_word("hello")
        self.pump_until(lambda: self.entry.get_text() == "hello",
                        "prefix before duplicate private control")
        self.harness.relay.duplicate_once = True
        self.harness.send_key(base.KEY_PAUSE)
        self.pump_until(lambda: len(self.harness.relay.probe_results) == 2,
                        "fault-injected duplicate control settled")
        self.assertEqual(self.entry.get_text(), "руддщ",
                         "one physical Pause must authorize at most one write")
        self.assertEqual(sum(result[0] == 0
                             for result in self.harness.relay.probe_results), 1)

    def test_receiver_death_and_late_ack_cannot_retarget(self) -> None:
        child = subprocess.Popen(
            [sys.executable, __file__, "--scratch-child"],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, env=os.environ.copy())

        def close_child() -> None:
            if child.poll() is None:
                child.terminate()
            child.wait(timeout=2)

        self.addCleanup(close_child)
        assert child.stdout is not None
        child_events: list[str] = []

        def read_child() -> None:
            for line in child.stdout:
                child_events.append(line.strip())

        threading.Thread(target=read_child, daemon=True).start()
        self.pump_until(lambda: child.poll() is not None or
                        any(line.startswith("READY ") for line in child_events),
                        "separate GTK receiver ready or failed")
        if child.poll() is not None:
            assert child.stderr is not None
            self.fail("separate GTK receiver exited early: " +
                      child.stderr.read()[-2000:])
        ready = next(line for line in child_events if line.startswith("READY "))
        target_window = int(ready.split()[1])
        self.harness.relay.target_window = target_window
        self.harness.relay.target_pid = child.pid
        self.xdo("windowfocus", "--sync", str(target_window))
        self.publish_active_window(target_window)
        self.harness.type_word("hello")
        self.pump_until(lambda: "TEXT hello" in child_events,
                        "separate receiver processes prefix")
        before = len(self.harness.relay.snapshot())
        self.harness.send_key(base.KEY_PAUSE)
        self.pump_until(lambda: "BOUNDARY" in child_events,
                        "receiver exits while private boundary is pending")
        self.pump_until(lambda: child.poll() is not None,
                        "separate receiver has exited")
        self.assertEqual(child.returncode, 23)
        self.pump_until(lambda: self.harness.relay.expired_boundaries == [5],
                        "unacknowledged dead receiver expires")
        self.harness.relay.expected_word = "hello"
        self.harness.relay.boundary_ack.set()
        self.assertIsNone(self.harness.relay.probe_result)
        self.assertFalse(any(event[1] == base.EV_KEY and
                             event[2] == base.KEY_PAUSE
                             for event in self.harness.relay.snapshot()[before:]))
        self.xdo("windowfocus", "--sync",
                 str(self.window.get_window().get_xid()))
        self.publish_active_window(self.window.get_window().get_xid())
        self.entry.grab_focus()
        self.harness.type_word("a")
        self.pump_until(lambda: self.entry.get_text() == "a",
                        "following input cannot retarget dead correction")


def scratch_child() -> None:
    import gi

    gi.require_version("Gtk", "3.0")
    gi.require_version("Gdk", "3.0")
    from gi.repository import Gdk, Gtk

    Gtk.init([])
    window = Gtk.Window(title="private-delivered-trigger-receiver")
    entry = Gtk.Entry()
    window.add(entry)
    entry.connect("changed", lambda widget: print("TEXT " + widget.get_text(),
                                                   flush=True))

    def on_property(_widget, event) -> bool:
        if event.atom.name() == BOUNDARY_ATOM:
            print("BOUNDARY", flush=True)
            os._exit(23)
        return False

    window.show_all()
    entry.grab_focus()
    gdk_window = window.get_window()
    gdk_window.set_events(gdk_window.get_events() |
                          Gdk.EventMask.PROPERTY_CHANGE_MASK)
    window.connect("property-notify-event", on_property)
    print("READY " + str(gdk_window.get_xid()), flush=True)
    Gtk.main()


if __name__ == "__main__":
    if sys.argv[1:] == ["--scratch-child"]:
        scratch_child()
        raise SystemExit(0)
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_editor_delivered_trigger_vm.py DRIVER")
    base.DRIVER = pathlib.Path(sys.argv[1])
    base.SESSION_BRIDGE = None
    if "PUNTO_DELIVERED_PROBE" not in os.environ:
        raise SystemExit("PUNTO_DELIVERED_PROBE is required")
    if not pathlib.Path(os.environ["PUNTO_DELIVERED_PROBE"]).is_file():
        raise SystemExit("missing private decision probe")
    if not base.DRIVER.is_file():
        raise SystemExit(f"missing prototype driver: {base.DRIVER}")
    if base.os.environ.get("PUNTO_EVENT_LOOP_E2E_INNER") != "1":
        raise SystemExit(base.run_in_sandbox(base.DRIVER,
                            test_script=pathlib.Path(__file__)))
    base.require_private_network_namespace()
    case = os.environ.get("PUNTO_DELIVERED_CASE", "default")
    if case == "entry":
        names = ["test_pause_lifecycle_emits_one_private_control"]
    elif case == "early":
        names = ["test_control_registered_before_last_key_handler_finishes"]
    elif case == "origin":
        names = ["test_textview_correction_uses_same_delivered_boundary",
                 "test_same_text_returns_to_original_field_without_retarget"]
    elif case == "delayed":
        names = ["test_delayed_im_commit_settles_before_probe"]
    elif case == "late":
        names = ["test_late_im_commit_does_not_revive_expired_boundary",
                 "test_expired_composition_keeps_native_space_and_tab"]
    elif case == "receiver_death":
        names = ["test_receiver_death_and_late_ack_cannot_retarget"]
    elif case == "installed_im":
        names = ["test_selected_installed_im_corrects_entry"]
    elif case == "protocol":
        names = ["test_source_cannot_forge_private_control_record",
                 "test_stale_origin_rejects_then_fresh_request_recovers",
                 "test_duplicate_private_record_never_mutates_twice"]
    elif case == "duplicate":
        names = ["test_duplicate_private_record_never_mutates_twice"]
    elif case == "stale":
        names = ["test_stale_origin_rejects_then_fresh_request_recovers"]
    elif case == "absent_receiver":
        names = ["test_absent_receiver_swallows_pause_and_keeps_next_input"]
    elif (case == "default" and
            os.environ.get("GTK_IM_MODULE") == "punto-delayed-commit" and
            int(os.environ.get("PUNTO_PROTO_IM_DELAY_MS", "50")) > 500):
        names = ["test_late_im_commit_does_not_revive_expired_boundary",
                 "test_expired_composition_keeps_native_space_and_tab"]
    elif case == "default":
        names = ["test_pause_lifecycle_emits_one_private_control",
                 "test_textview_correction_uses_same_delivered_boundary",
                 "test_control_registered_before_last_key_handler_finishes",
                 "test_same_text_returns_to_original_field_without_retarget"]
        if os.environ.get("GTK_IM_MODULE") == "punto-delayed-commit":
            names.append("test_delayed_im_commit_settles_before_probe")
    else:
        raise SystemExit(f"unknown private scenario selector: {case}")
    suite = unittest.TestSuite(DeliveredTriggerTests(name) for name in names)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
