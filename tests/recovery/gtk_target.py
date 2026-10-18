#!/usr/bin/env python3
"""Scratch GTK Entry/TextView observer for an isolated Punto probe session."""

from __future__ import annotations

import json
import base64
import sys
import ctypes as C
import time

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
from gi.repository import Gdk, GLib, Gtk  # noqa: E402

from probe import ObservationProtocol


CLIPBOARD_METADATA = {"TARGETS", "TIMESTAMP", "MULTIPLE", "SAVE_TARGETS"}


class ClipboardUnavailable(ValueError):
    pass


class ClipboardPublicationWatch:
    """Mechanical XFixes history on a private, unmapped watch window."""
    class Event(C.Structure):
        _fields_ = [("type", C.c_int), ("serial", C.c_ulong), ("send_event", C.c_int),
                    ("display", C.c_void_p), ("window", C.c_ulong), ("subtype", C.c_int),
                    ("owner", C.c_ulong), ("selection", C.c_ulong),
                    ("timestamp", C.c_ulong), ("selection_timestamp", C.c_ulong)]

    def __init__(self):
        self.x, self.fix = C.CDLL("libX11.so.6"), C.CDLL("libXfixes.so.3")
        signatures = {
            "XOpenDisplay": ([C.c_char_p], C.c_void_p),
            "XDefaultRootWindow": ([C.c_void_p], C.c_ulong),
            "XCreateSimpleWindow": ([C.c_void_p, C.c_ulong, C.c_int, C.c_int, C.c_uint,
                                      C.c_uint, C.c_uint, C.c_ulong, C.c_ulong], C.c_ulong),
            "XInternAtom": ([C.c_void_p, C.c_char_p, C.c_int], C.c_ulong),
            "XGetSelectionOwner": ([C.c_void_p, C.c_ulong], C.c_ulong),
            "XSync": ([C.c_void_p, C.c_int], C.c_int),
            "XEventsQueued": ([C.c_void_p, C.c_int], C.c_int),
            "XNextEvent": ([C.c_void_p, C.c_void_p], C.c_int),
            "XDestroyWindow": ([C.c_void_p, C.c_ulong], C.c_int),
            "XCloseDisplay": ([C.c_void_p], C.c_int),
        }
        for name, (args, result) in signatures.items():
            method = getattr(self.x, name)
            method.argtypes, method.restype = args, result
        self.fix.XFixesQueryExtension.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p]
        self.fix.XFixesSelectSelectionInput.argtypes = [C.c_void_p, C.c_ulong, C.c_ulong, C.c_ulong]
        self.display = self.x.XOpenDisplay(None)
        if not self.display:
            raise ValueError("independent clipboard X display unavailable")
        event, error = C.c_int(), C.c_int()
        if not self.fix.XFixesQueryExtension(self.display, C.byref(event), C.byref(error)):
            raise ValueError("clipboard XFixes unavailable")
        self.event_type = event.value
        self.window = self.x.XCreateSimpleWindow(self.display, self.x.XDefaultRootWindow(self.display),
                                                 0, 0, 1, 1, 0, 0, 0)
        self.selection = self.x.XInternAtom(self.display, b"CLIPBOARD", 0)
        self.generation, self.timestamp = 0, 0
        self.fix.XFixesSelectSelectionInput(self.display, self.window, self.selection, 1 | 2 | 4)
        self.owner = int(self.x.XGetSelectionOwner(self.display, self.selection))
        self.x.XSync(self.display, 0)

    def fence(self, deadline_ns):
        if time.monotonic_ns() >= deadline_ns:
            raise TimeoutError("clipboard shared deadline expired")
        # XSync's reply orders all notifications before this server fence.
        self.x.XSync(self.display, 0)
        count = 0
        while self.x.XEventsQueued(self.display, 0):  # QueuedAlready; do not read post-fence traffic.
            if count >= 64 or time.monotonic_ns() >= deadline_ns:
                raise ValueError("clipboard publication history incomplete")
            raw = C.create_string_buffer(24 * C.sizeof(C.c_long))
            self.x.XNextEvent(self.display, raw)
            item = C.cast(raw, C.POINTER(self.Event)).contents
            if (item.type != self.event_type or item.window != self.window or item.selection != self.selection
                    or item.send_event or item.subtype not in (0, 1, 2)):
                raise ValueError("unexpected clipboard watch event")
            self.generation += 1
            self.timestamp = int(item.selection_timestamp)
            self.owner = int(item.owner)
            count += 1
        if time.monotonic_ns() >= deadline_ns:
            raise TimeoutError("clipboard shared deadline expired")
        return {"owner": self.owner, "publication_generation": self.generation, "timestamp": self.timestamp}

    def close(self):
        self.x.XDestroyWindow(self.display, self.window)
        self.x.XCloseDisplay(self.display)


def independent_clipboard_snapshot(clipboard, watch, deadline_ns):
    """One independent client's exact data snapshot and real publication fences."""
    def remaining():
        if time.monotonic_ns() >= deadline_ns:
            raise TimeoutError("clipboard shared deadline expired")

    def targets():
        remaining()
        data = clipboard.wait_for_contents(Gdk.Atom.intern("TARGETS", False))
        if data is None:
            raise ClipboardUnavailable("clipboard TARGETS conversion unavailable")
        available, atoms = data.get_targets()
        names = sorted(atom.name() for atom in atoms) if available else []
        if not available:
            raise ValueError("invalid clipboard TARGETS metadata")
        if not names or len(names) > 16 or len(names) != len(set(names)):
            raise ValueError("invalid clipboard advertised targets")
        return names

    def timestamp():
        remaining()
        data = clipboard.wait_for_contents(Gdk.Atom.intern("TIMESTAMP", False))
        if data is None:
            raise ClipboardUnavailable("clipboard TIMESTAMP conversion unavailable")
        raw = data.get_data()
        if (data.get_data_type().name() != "INTEGER" or data.get_format() != 32
                or data.get_length() != C.sizeof(C.c_ulong) or len(raw) != C.sizeof(C.c_ulong)):
            raise ValueError("invalid clipboard TIMESTAMP")
        value = int.from_bytes(raw, sys.byteorder)
        if not 0 < value <= 0xFFFFFFFF:
            raise ValueError("invalid clipboard TIMESTAMP value")
        return value

    before = watch.fence(deadline_ns)
    unavailable = None
    try:
        names = targets()
        first_timestamp = timestamp()
        # Reuse the established plain-data reader on this client's GDK connection.
        representations = ScratchTarget.clipboard(None, clipboard, names=names)
        after_names, last_timestamp = targets(), timestamp()
    except ClipboardUnavailable as error:
        unavailable = error
    after = watch.fence(deadline_ns)
    remaining()
    if after["publication_generation"] < before["publication_generation"]:
        raise ValueError("clipboard publication generation regressed")
    if after["publication_generation"] > before["publication_generation"]:
        return {"publication_pending": True, "before_generation": before["publication_generation"],
                "after_generation": after["publication_generation"]}
    if unavailable:
        unavailable.add_note(
            f"clipboard_fence before_owner={before['owner']} "
            f"before_generation={before['publication_generation']} before_timestamp={before['timestamp']} "
            f"after_owner={after['owner']} "
            f"after_generation={after['publication_generation']} after_timestamp={after['timestamp']}")
        raise unavailable
    if not before["owner"] or before["owner"] != after["owner"] or names != after_names:
        raise ValueError("clipboard changed without complete publication evidence")
    if first_timestamp != last_timestamp or (after["timestamp"] and last_timestamp != after["timestamp"]):
        raise ValueError("clipboard TIMESTAMP disagrees with publication history")
    if set(representations) != set(names) - CLIPBOARD_METADATA:
        raise ValueError("clipboard data targets do not match advertised catalogue")
    preferred = next((name for name in ("UTF8_STRING", "STRING", "TEXT", "text/plain") if name in representations), None)
    if preferred is None:
        raise ValueError("clipboard default plain representation unavailable")
    return {"owner": after["owner"], "publication_generation": after["publication_generation"],
            "targets": "\n".join(names) + "\n", "representations": representations,
            "bytes_base64": representations[preferred][2]}


class ScratchTarget:
    def __init__(self) -> None:
        self.protocol = ObservationProtocol("gtk")
        self.window = Gtk.Window(title="Punto recovery scratch target")
        self.window.set_default_size(640, 260)
        self.window.connect("destroy", Gtk.main_quit)
        self.keys = {"tab_down": 0, "tab_up": 0, "z_down": 0, "z_up": 0}
        self.window.connect("key-press-event", self._key, "down")
        self.window.connect("key-release-event", self._key, "up")

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        box.set_border_width(16)
        self.entry = Gtk.Entry(name="punto-probe-entry")
        self.text_view = Gtk.TextView(name="punto-probe-text-view")
        box.pack_start(self.entry, False, False, 0)
        box.pack_start(self.text_view, True, True, 0)
        self.window.add(box)

        self.entry.connect("changed", self._changed)
        self.entry.connect("notify::has-focus", self._changed)
        text_buffer = self.text_view.get_buffer()
        text_buffer.connect("changed", self._changed)
        text_buffer.connect("mark-set", self._changed)
        self.text_view.connect("notify::has-focus", self._changed)

    def _changed(self, *_args: object) -> None:
        self.protocol.record_state_change()

    def _key(self, _widget, event, phase):
        translated = Gdk.Keymap.get_for_display(self.window.get_display()).translate_keyboard_state(
            event.hardware_keycode, Gdk.ModifierType(0), 0)
        if translated[0]:
            name = Gdk.keyval_name(translated[1])
            if name in ("Tab", "z"):
                self.keys[("tab" if name == "Tab" else "z") + "_" + phase] += 1
        return False

    def clipboard(self, clipboard=None, *, names=None):
        clipboard = clipboard if clipboard is not None else Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD)
        if names is None:
            available, atoms = clipboard.wait_for_targets()
            names = [atom.name() for atom in atoms] if available else []
        metadata = {"TARGETS", "TIMESTAMP", "MULTIPLE", "SAVE_TARGETS"}
        plain = {"UTF8_STRING", "STRING", "TEXT", "COMPOUND_TEXT", "text/plain", "text/plain;charset=utf-8", "text/plain;charset=UTF-8"}
        if not names or len(names) > 16 or set(names) - metadata - plain:
            raise ValueError("feature cohort requires portable plain clipboard targets")
        representations = {}
        for name in names:
            if name in metadata:
                continue
            data = clipboard.wait_for_contents(Gdk.Atom.intern(name, False))
            if data is None:
                raise ClipboardUnavailable("clipboard representation conversion unavailable")
            if data.get_length() < 0 or data.get_length() > 65536:
                raise ValueError("clipboard representation unavailable or too large")
            representations[name] = [data.get_data_type().name(), data.get_format(),
                                     base64.b64encode(data.get_data()).decode()]
        if not representations:
            raise ValueError("clipboard has no plain data representation")
        return representations

    def prepare(self, target: str, text: str) -> None:
        self.protocol.ensure_prepare_allowed()
        if target == "entry":
            self.entry.set_text(text)
            self.entry.set_position(len(text))
            self.entry.grab_focus()
        elif target == "text_view":
            text_buffer = self.text_view.get_buffer()
            text_buffer.set_text(text)
            text_buffer.place_cursor(text_buffer.get_end_iter())
            self.text_view.grab_focus()
        else:
            raise ValueError(f"unknown target: {target}")

    def _focus_target(self) -> str:
        if self.entry.has_focus():
            return "gtk_entry"
        if self.text_view.has_focus():
            return "gtk_text_view"
        return "none"

    def snapshot(self, target: str) -> dict[str, object]:
        if target == "entry":
            bounds = self.entry.get_selection_bounds()
            if len(bounds) == 3:
                has_selection, start, end = bounds
            else:
                has_selection = bool(bounds)
                start, end = bounds if has_selection else (0, 0)
            if not has_selection:
                start = end = self.entry.get_position()
            text_buffer = self.text_view.get_buffer()
            other_start, other_end = text_buffer.get_bounds()
            state = {
                "text": self.entry.get_text(),
                "selection": [start, end],
                "focus_target": self._focus_target(),
                "following_text": text_buffer.get_text(other_start, other_end, True),
                "keys": dict(self.keys),
            }
            return self.protocol.snapshot(state)
        if target == "text_view":
            text_buffer = self.text_view.get_buffer()
            start, end = text_buffer.get_bounds()
            selected = text_buffer.get_selection_bounds()
            if selected:
                selection = [selected[0].get_offset(), selected[1].get_offset()]
            else:
                offset = text_buffer.get_iter_at_mark(text_buffer.get_insert()).get_offset()
                selection = [offset, offset]
            state = {
                "text": text_buffer.get_text(start, end, True),
                "selection": selection,
                "focus_target": self._focus_target(),
                "following_text": self.entry.get_text(),
                "keys": dict(self.keys),
            }
            return self.protocol.snapshot(state)
        raise ValueError(f"unknown target: {target}")

    def handle_line(self, line: str) -> bool:
        try:
            command = json.loads(line)
            action = command.get("action")
            if action == "identity":
                result = {
                    "ok": True,
                    "observer_id": self.protocol.observer_id,
                    "observer_instance_id": self.protocol.observer_instance_id,
                }
            elif action == "prepare":
                self.prepare(command["target"], command.get("text", ""))
                result: dict[str, object] = {"ok": True, "state_generation": self.protocol.state_generation}
            elif action == "clipboard":
                result = {"ok": True, "representations": self.clipboard()}
            elif action == "arm":
                self.protocol.arm(
                    command["run_id"],
                    command["case_id"],
                    command["attempt_id"],
                    command.get("clock_mapping"),
                )
                result = {"ok": True, "armed": True, "state_generation": self.protocol.state_generation}
            elif action == "snapshot":
                result = {"ok": True, **self.snapshot(command["target"])}
            elif action == "complete":
                self.protocol.complete()
                result = {"ok": True, "armed": False, "state_generation": self.protocol.state_generation}
            else:
                raise ValueError(f"unknown action: {action}")
        except Exception as exc:  # A structured observer error is part of the probe evidence.
            result = {"ok": False, "error": str(exc), "state_generation": self.protocol.state_generation}
        print(json.dumps(result, ensure_ascii=False), flush=True)
        return True


def main() -> int:
    target = ScratchTarget()
    GLib.io_add_watch(sys.stdin, GLib.IO_IN, lambda stream, _condition: target.handle_line(stream.readline()))
    target.window.show_all()
    print(json.dumps({"ok": True, "ready": True, "state_generation": target.protocol.state_generation}), flush=True)
    Gtk.main()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
