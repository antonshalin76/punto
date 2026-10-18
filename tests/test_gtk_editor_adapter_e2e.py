#!/usr/bin/env python3
"""Native GTK3 product contract for the editor-owned CompareApply adapter."""

from __future__ import annotations

import json
import hashlib
import os
import pathlib
import select
import socket
import subprocess
import sys
import time


def read_json(process: subprocess.Popen[str], timeout: float = 3.0) -> dict[str, object]:
    ready, _, _ = select.select([process.stdout], [], [], timeout)
    if not ready:
        raise AssertionError("timed out waiting for child response")
    line = process.stdout.readline()
    if not line:
        raise AssertionError(f"child exited early: {process.poll()}")
    return json.loads(line)


def command(process: subprocess.Popen[str], line: str) -> dict[str, object]:
    process.stdin.write(line + "\n")
    process.stdin.flush()
    return read_json(process)


def adapter_request(pid: int, payload: dict[str, object]) -> dict[str, object]:
    name = f"\0punto-editor-adapter-{os.getuid()}-{pid}.sock"
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(1.0)
        connection.connect(name)
        connection.sendall((json.dumps(payload, ensure_ascii=False) + "\n").encode())
        connection.shutdown(socket.SHUT_WR)
        response = bytearray()
        while not response.endswith(b"\n"):
            data = connection.recv(4096)
            if not data:
                raise AssertionError("editor adapter closed without response")
            response.extend(data)
        return json.loads(response)


def adapter_send_without_read(pid: int, payload: dict[str, object]) -> None:
    name = f"\0punto-editor-adapter-{os.getuid()}-{pid}.sock"
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(1.0)
        connection.connect(name)
        connection.sendall((json.dumps(payload, ensure_ascii=False) + "\n").encode())
        connection.shutdown(socket.SHUT_WR)


def apply_identity(prepared: dict[str, object], replacement: str,
                   window: int, deadline: int) -> str:
    fields = [str(prepared["editor_instance"]), "word", str(window),
              str(prepared["widget_epoch"]), str(prepared["context_generation"]),
              str(prepared["revision"]), str(prepared["start"]),
              str(prepared["end"]), str(prepared["fragment"]), replacement,
              str(deadline)]
    canonical = "".join(f"{len(field.encode())}:{field}|" for field in fields)
    return hashlib.sha256(canonical.encode()).hexdigest()


def test_gdk_tab_gate(fixture: subprocess.Popen[str], xid: int, pid: int) -> None:
    """Adapter seam only: external XTEST Tab is not Punto output evidence."""
    assert command(fixture, json.dumps({"action": "set_entry_visibility", "visible": True}))["ok"]
    assert command(fixture, json.dumps({"action": "setup", "target": "entry",
                                        "text": "aBc", "start": 3, "end": 3}))["ok"]
    operation_id = "r37-adapter-test-1"
    deadline = int(time.monotonic() * 1000) + 2000
    prepared = adapter_request(pid, {"protocol": 1, "action": "prepare",
                                     "operation_id": operation_id, "mode": "word",
                                     "expected_fragment": "aBc", "window": xid,
                                     "not_after_ms": deadline})
    assert prepared.get("ok") is True, prepared
    identity = apply_identity(prepared, "фИс", xid, deadline)
    frame = {"frame_id": "adapter-test:stream-1:frame-1",
             "frame_digest": hashlib.sha256(b"Tab-down-frame").hexdigest(),
             "trigger_index": 0}
    base = {"protocol": 1, "operation_id": operation_id,
            "editor_instance": prepared["editor_instance"], "mode": "word",
            "window": xid, "widget_epoch": prepared["widget_epoch"],
            "context_generation": prepared["context_generation"],
            "expected_revision": prepared["revision"],
            "start": prepared["start"], "end": prepared["end"],
            "expected_fragment": "aBc", "replacement": "фИс",
            "not_after_ms": deadline, "command_identity": identity, **frame}
    assert command(fixture, json.dumps({"action": "stall_next_entry_delete"}))["ok"]
    armed = adapter_request(pid, {**base, "action": "arm_tab"})
    assert armed.get("status") == "ARMED", armed
    token = armed.get("barrier_token")
    assert isinstance(token, str) and token, armed
    status_request = {**base, "action": "tab_status", "barrier_token": token}
    until = time.monotonic() + 1.0
    while time.monotonic() < until:
        state = adapter_request(pid, status_request)
        if state.get("status") == "READY":
            break
        time.sleep(0.005)
    else:
        raise AssertionError(f"GDK marker did not enter protected dispatch: {state}")
    subprocess.run(["xdotool", "key", "Tab"], check=True)
    before = command(fixture, json.dumps({"action": "snapshot"}))
    assert before["focused"] == "entry" and before["entry_text"] == "aBc", before
    decided = adapter_request(pid, {**base, "action": "decide_tab",
                                    "barrier_token": token})
    assert decided.get("status") == "DECIDED", decided
    until = time.monotonic() + 1.0
    while time.monotonic() < until:
        during = command(fixture, json.dumps({"action": "snapshot"}))
        if during["delete_stalled"]:
            break
        time.sleep(0.005)
    else:
        raise AssertionError(f"delete did not enter nested loop: {during}")
    assert during["focused"] == "entry", during
    assert command(fixture, json.dumps({"action": "release_stalled_entry_delete"}))["ok"]
    until = time.monotonic() + 1.0
    while time.monotonic() < until:
        after = command(fixture, json.dumps({"action": "snapshot"}))
        if after["focused"] == "text_view":
            break
        time.sleep(0.005)
    else:
        raise AssertionError(f"original Tab was not processed: {after}")
    assert after["entry_text"] == "фИс", after
    terminal = adapter_request(pid, status_request)
    assert terminal.get("status") == "APPLIED", terminal
    assert terminal.get("lifecycle") == "QUIESCENT", terminal
    assert adapter_request(pid, {**base, "action": "decide_tab",
                                 "barrier_token": token})["status"] == "REJECTED"
    assert adapter_request(pid, {**status_request,
                                 "frame_digest": "0" * 64})["status"] == "REJECTED"

    assert command(fixture, json.dumps({"action": "setup", "target": "entry",
                                        "text": "aBc", "start": 3, "end": 3}))["ok"]
    second_id = "r37-adapter-test-expire"
    second_deadline = int(time.monotonic() * 1000) + 400
    second_prepared = adapter_request(pid, {"protocol": 1, "action": "prepare",
                                            "operation_id": second_id, "mode": "word",
                                            "expected_fragment": "aBc", "window": xid,
                                            "not_after_ms": second_deadline})
    assert second_prepared.get("ok") is True, second_prepared
    second_base = {**base, "operation_id": second_id,
                   "editor_instance": second_prepared["editor_instance"],
                   "widget_epoch": second_prepared["widget_epoch"],
                   "context_generation": second_prepared["context_generation"],
                   "expected_revision": second_prepared["revision"],
                   "start": second_prepared["start"], "end": second_prepared["end"],
                   "not_after_ms": second_deadline,
                   "command_identity": apply_identity(second_prepared, "фИс", xid,
                                                      second_deadline),
                   "frame_id": "adapter-test:stream-1:frame-2"}
    second_armed = adapter_request(pid, {**second_base, "action": "arm_tab"})
    assert second_armed.get("status") == "ARMED", second_armed
    second_token = second_armed["barrier_token"]
    second_status = {**second_base, "action": "tab_status",
                     "barrier_token": second_token}
    until = time.monotonic() + 0.2
    while time.monotonic() < until:
        state = adapter_request(pid, second_status)
        if state.get("status") == "READY":
            break
        time.sleep(0.005)
    else:
        raise AssertionError(f"second marker did not enter: {state}")
    subprocess.run(["xdotool", "key", "Tab"], check=True)
    assert command(fixture, json.dumps({"action": "snapshot"}))["focused"] == "entry"
    time.sleep(0.45)
    expired = adapter_request(pid, second_status)
    assert expired["status"] == "REJECTED", expired
    assert expired.get("lifecycle") == "QUIESCENT", expired
    observed = command(fixture, json.dumps({"action": "snapshot"}))
    assert observed["entry_text"] == "aBc" and observed["focused"] == "text_view", observed

    assert command(fixture, json.dumps({"action": "setup", "target": "entry",
                                        "text": "aBc", "start": 3, "end": 3}))["ok"]
    recovery_id = "r37-adapter-test-recovery"
    recovery_deadline = int(time.monotonic() * 1000) + 1000
    recovery_prepared = adapter_request(pid, {"protocol": 1, "action": "prepare",
                                              "operation_id": recovery_id,
                                              "mode": "word",
                                              "expected_fragment": "aBc", "window": xid,
                                              "not_after_ms": recovery_deadline})
    assert recovery_prepared.get("ok") is True, recovery_prepared
    recovery = adapter_request(pid, {**base, "action": "compare_apply",
                                     "operation_id": recovery_id,
                                     "editor_instance": recovery_prepared["editor_instance"],
                                     "widget_epoch": recovery_prepared["widget_epoch"],
                                     "context_generation": recovery_prepared["context_generation"],
                                     "expected_revision": recovery_prepared["revision"],
                                     "start": recovery_prepared["start"],
                                     "end": recovery_prepared["end"],
                                     "not_after_ms": recovery_deadline,
                                     "command_identity": apply_identity(recovery_prepared, "фИс",
                                                                        xid, recovery_deadline)})
    assert recovery["status"] == "APPLIED", recovery
    assert command(fixture, json.dumps({"action": "snapshot"}))["entry_text"] == "фИс"

    assert command(fixture, json.dumps({"action": "setup", "target": "entry",
                                        "text": "aBc", "start": 3, "end": 3}))["ok"]
    lost_id = "r37-adapter-test-lost-ack"
    lost_deadline = int(time.monotonic() * 1000) + 1000
    lost_prepared = adapter_request(pid, {"protocol": 1, "action": "prepare",
                                        "operation_id": lost_id, "mode": "word",
                                        "expected_fragment": "aBc", "window": xid,
                                        "not_after_ms": lost_deadline})
    assert lost_prepared.get("ok") is True, lost_prepared
    lost_base = {**base, "operation_id": lost_id,
                 "editor_instance": lost_prepared["editor_instance"],
                 "widget_epoch": lost_prepared["widget_epoch"],
                 "context_generation": lost_prepared["context_generation"],
                 "expected_revision": lost_prepared["revision"],
                 "start": lost_prepared["start"], "end": lost_prepared["end"],
                 "not_after_ms": lost_deadline,
                 "command_identity": apply_identity(lost_prepared, "фИс", xid,
                                                    lost_deadline),
                 "frame_id": "adapter-test:stream-1:frame-3"}
    lost_armed = adapter_request(pid, {**lost_base, "action": "arm_tab"})
    assert lost_armed["status"] == "ARMED", lost_armed
    lost_token = lost_armed["barrier_token"]
    lost_status = {**lost_base, "action": "tab_status",
                   "barrier_token": lost_token}
    until = time.monotonic() + 0.4
    while time.monotonic() < until:
        state = adapter_request(pid, lost_status)
        if state["status"] == "READY":
            break
        time.sleep(0.005)
    else:
        raise AssertionError(f"lost-ACK marker did not enter: {state}")
    subprocess.run(["xdotool", "key", "Tab"], check=True)
    adapter_send_without_read(pid, {**lost_base, "action": "decide_tab",
                                    "barrier_token": lost_token})
    until = time.monotonic() + 0.5
    while time.monotonic() < until:
        terminal = adapter_request(pid, lost_status)
        if terminal["status"] == "APPLIED":
            break
        time.sleep(0.005)
    else:
        raise AssertionError(f"lost decision ACK did not settle: {terminal}")
    assert terminal.get("lifecycle") == "QUIESCENT", terminal
    observed = command(fixture, json.dumps({"action": "snapshot"}))
    assert observed["entry_text"] == "фИс" and observed["focused"] == "text_view", observed


def fixture_main() -> int:
    import gi

    gi.require_version("Gtk", "3.0")
    gi.require_version("Gdk", "3.0")
    gi.require_version("GdkX11", "3.0")
    from gi.repository import Gdk, GdkX11, GLib, Gtk

    window = Gtk.Window(title="Punto editor adapter contract")
    window.set_default_size(500, 240)
    box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
    entry = Gtk.Entry(name="ordinary-entry")
    text_view = Gtk.TextView(name="ordinary-text-view")
    focus_sink = Gtk.Button(label="focus sink")
    box.pack_start(entry, False, False, 0)
    box.pack_start(text_view, True, True, 0)
    box.pack_start(focus_sink, False, False, 0)
    window.add(box)
    window.show_all()

    state: dict[str, object] = {"entry": entry, "text_view": text_view}
    clipboard = Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD)
    clipboard.set_text("clipboard-sentinel", -1)
    clipboard.store()

    def active_widget():
        return state["entry"] if state.get("target") == "entry" else state["text_view"]

    def setup(target: str, text: str, start: int, end: int) -> None:
        state["target"] = target
        widget = state[target]
        window.present()
        window.get_window().focus(Gdk.CURRENT_TIME)
        widget.grab_focus()
        while Gtk.events_pending():
            Gtk.main_iteration_do(False)
        if target == "entry":
            widget.set_text(text)
            if end > start:
                widget.select_region(start, end)
            else:
                widget.set_position(end)
        else:
            buffer = widget.get_buffer()
            buffer.set_text(text)
            begin = buffer.get_iter_at_offset(start)
            finish = buffer.get_iter_at_offset(end)
            if end > start:
                buffer.select_range(begin, finish)
            else:
                buffer.place_cursor(finish)
        while Gtk.events_pending():
            Gtk.main_iteration_do(False)

    def snapshot() -> dict[str, object]:
        target = str(state["target"])
        widget = active_widget()
        if target == "entry":
            text = widget.get_text()
            bounds = widget.get_selection_bounds()
            selection = list(bounds) if bounds else [widget.get_position(), widget.get_position()]
        else:
            buffer = widget.get_buffer()
            begin, finish = buffer.get_bounds()
            text = buffer.get_text(begin, finish, True)
            bounds = buffer.get_selection_bounds()
            if bounds:
                selection = [bounds[0].get_offset(), bounds[1].get_offset()]
            else:
                offset = buffer.get_iter_at_mark(buffer.get_insert()).get_offset()
                selection = [offset, offset]
        return {
            "ok": True,
            "text": text,
            "selection": selection,
            "clipboard": clipboard.wait_for_text(),
            "target": target,
            "entry_text": state["entry"].get_text(),
            "focused": (
                "entry" if window.get_focus() == state["entry"] else
                "text_view" if window.get_focus() == state["text_view"] else
                "other"
            ),
            "delete_stalled": state.get("delete_stalled", False),
        }

    def handle(stream, _condition):
        line = stream.readline()
        if not line:
            Gtk.main_quit()
            return False
        try:
            request = json.loads(line)
            action = request["action"]
            if action == "setup":
                setup(request["target"], request["text"], request["start"], request["end"])
                result = {"ok": True}
            elif action == "snapshot":
                result = snapshot()
            elif action == "recreate_entry":
                old = state["entry"]
                text = request["text"]
                box.remove(old)
                old.destroy()
                replacement = Gtk.Entry(name="ordinary-entry-recreated")
                box.pack_start(replacement, False, False, 0)
                box.reorder_child(replacement, 0)
                replacement.show()
                state["entry"] = replacement
                setup("entry", text, len(text), len(text))
                result = {"ok": True}
            elif action == "set_entry_visibility":
                state["entry"].set_visibility(bool(request["visible"]))
                result = {"ok": True}
            elif action == "replace_text_buffer":
                state["text_view"].set_buffer(Gtk.TextBuffer())
                text = request["text"]
                setup("text_view", text, len(text), len(text))
                result = {"ok": True}
            elif action == "set_context":
                target = str(request["target"])
                widget = state[target]
                start = int(request["start"])
                end = int(request["end"])
                if target == "entry":
                    if end > start:
                        widget.select_region(start, end)
                    else:
                        widget.set_position(end)
                else:
                    buffer = widget.get_buffer()
                    begin = buffer.get_iter_at_offset(start)
                    finish = buffer.get_iter_at_offset(end)
                    if end > start:
                        buffer.select_range(begin, finish)
                    else:
                        buffer.place_cursor(finish)
                while Gtk.events_pending():
                    Gtk.main_iteration_do(False)
                result = {"ok": True}
            elif action == "focus_roundtrip":
                target = str(state["target"])
                focus_sink.grab_focus()
                while Gtk.events_pending():
                    Gtk.main_iteration_do(False)
                state[target].grab_focus()
                while Gtk.events_pending():
                    Gtk.main_iteration_do(False)
                result = {"ok": True}
            elif action == "block_next_entry_delete":
                armed = {"value": True}

                def block_delete(widget, _start, _end):
                    if armed["value"]:
                        armed["value"] = False
                        widget.stop_emission_by_name("delete-text")

                state["entry"].connect("delete-text", block_delete)
                result = {"ok": True}
            elif action == "stall_next_entry_delete":
                armed = {"value": True}
                state["release_delete"] = False

                def stall_delete(_widget, _start, _end):
                    if not armed["value"]:
                        return
                    armed["value"] = False
                    state["delete_stalled"] = True
                    deadline = time.monotonic() + 4.0
                    while not state["release_delete"] and time.monotonic() < deadline:
                        if Gtk.events_pending():
                            Gtk.main_iteration_do(False)
                        else:
                            time.sleep(0.002)
                    state["delete_stalled"] = False

                state["entry"].connect("delete-text", stall_delete)
                result = {"ok": True}
            elif action == "release_stalled_entry_delete":
                state["release_delete"] = True
                result = {"ok": True}
            elif action == "redirect_next_entry_delete":
                armed = {"value": True}
                wrong_start = int(request["start"])
                wrong_end = int(request["end"])

                def redirect_delete(widget, _start, _end):
                    if not armed["value"]:
                        return
                    armed["value"] = False
                    widget.stop_emission_by_name("delete-text")
                    widget.delete_text(wrong_start, wrong_end)

                state["entry"].connect("delete-text", redirect_delete)
                result = {"ok": True}
            elif action == "insert_on_next_entry_delete":
                armed = {"value": True}

                def insert_during_delete(widget, _start, _end):
                    if not armed["value"]:
                        return
                    armed["value"] = False
                    position = 0
                    widget.insert_text("Q", position)

                state["entry"].connect("delete-text", insert_during_delete)
                result = {"ok": True}
            elif action == "redirect_next_textview_delete":
                buffer = state["text_view"].get_buffer()
                armed = {"value": True}
                wrong_start = int(request["start"])
                wrong_end = int(request["end"])

                def redirect_buffer_delete(active_buffer, _start, _end):
                    if not armed["value"]:
                        return
                    armed["value"] = False
                    active_buffer.stop_emission_by_name("delete-range")
                    begin = active_buffer.get_iter_at_offset(wrong_start)
                    finish = active_buffer.get_iter_at_offset(wrong_end)
                    active_buffer.delete(begin, finish)

                buffer.connect("delete-range", redirect_buffer_delete)
                result = {"ok": True}
            elif action == "mutate_next_textview_delete_iters":
                buffer = state["text_view"].get_buffer()
                armed = {"value": True}
                wrong_start = int(request["start"])
                wrong_end = int(request["end"])

                def mutate_delete_iters(active_buffer, begin, finish):
                    if not armed["value"]:
                        return
                    armed["value"] = False
                    begin.assign(active_buffer.get_iter_at_offset(wrong_start))
                    finish.assign(active_buffer.get_iter_at_offset(wrong_end))

                buffer.connect("delete-range", mutate_delete_iters)
                result = {"ok": True}
            elif action == "mutate_next_textview_insert_iter":
                buffer = state["text_view"].get_buffer()
                armed = {"value": True}
                wrong_offset = int(request["offset"])

                def mutate_insert_iter(active_buffer, location, _text, _length):
                    if not armed["value"]:
                        return
                    armed["value"] = False
                    location.assign(active_buffer.get_iter_at_offset(wrong_offset))

                buffer.connect("insert-text", mutate_insert_iter)
                result = {"ok": True}
            elif action == "replace_textview_with_pre_delete_mutator":
                old = state["text_view"]
                text = str(request["text"])
                selection_start = int(request["selection_start"])
                selection_end = int(request["selection_end"])
                wrong_start = int(request["start"])
                wrong_end = int(request["end"])
                replacement = Gtk.TextView(name="pre-mutator-text-view")
                buffer = replacement.get_buffer()
                buffer.set_text(text)
                armed = {"value": True}

                def mutate_delete_iters(active_buffer, begin, finish):
                    if not armed["value"]:
                        return
                    armed["value"] = False
                    begin.assign(active_buffer.get_iter_at_offset(wrong_start))
                    finish.assign(active_buffer.get_iter_at_offset(wrong_end))

                buffer.connect("delete-range", mutate_delete_iters)
                box.remove(old)
                old.destroy()
                box.pack_start(replacement, True, True, 0)
                box.reorder_child(replacement, 1)
                replacement.show()
                state["text_view"] = replacement
                state["target"] = "text_view"
                window.present()
                window.get_window().focus(Gdk.CURRENT_TIME)
                replacement.grab_focus()
                begin = buffer.get_iter_at_offset(selection_start)
                finish = buffer.get_iter_at_offset(selection_end)
                buffer.select_range(begin, finish)
                while Gtk.events_pending():
                    Gtk.main_iteration_do(False)
                result = {"ok": True}
            elif action == "replace_textview_with_pre_insert_mutator":
                old = state["text_view"]
                text = str(request["text"])
                selection_start = int(request["selection_start"])
                selection_end = int(request["selection_end"])
                wrong_offset = int(request["offset"])
                replacement = Gtk.TextView(name="pre-insert-mutator-text-view")
                buffer = replacement.get_buffer()
                buffer.set_text(text)
                armed = {"value": True}

                def mutate_insert_iter(active_buffer, location, _text, _length):
                    if not armed["value"]:
                        return
                    armed["value"] = False
                    location.assign(active_buffer.get_iter_at_offset(wrong_offset))

                buffer.connect("insert-text", mutate_insert_iter)
                box.remove(old)
                old.destroy()
                box.pack_start(replacement, True, True, 0)
                box.reorder_child(replacement, 1)
                replacement.show()
                state["text_view"] = replacement
                state["target"] = "text_view"
                window.present()
                window.get_window().focus(Gdk.CURRENT_TIME)
                replacement.grab_focus()
                begin = buffer.get_iter_at_offset(selection_start)
                finish = buffer.get_iter_at_offset(selection_end)
                buffer.select_range(begin, finish)
                while Gtk.events_pending():
                    Gtk.main_iteration_do(False)
                result = {"ok": True}
            elif action == "replace_buffer_on_delete":
                view = state["text_view"]
                old_buffer = view.get_buffer()
                text = str(request["text"])
                start = int(request["start"])
                end = int(request["end"])
                armed = {"value": True}

                def replace_buffer(buffer, _begin, _finish):
                    if not armed["value"]:
                        return
                    armed["value"] = False
                    replacement = Gtk.TextBuffer()
                    replacement.set_text(text)
                    view.set_buffer(replacement)
                    begin = replacement.get_iter_at_offset(start)
                    finish = replacement.get_iter_at_offset(end)
                    replacement.select_range(begin, finish)
                    buffer.stop_emission_by_name("delete-range")

                old_buffer.connect("delete-range", replace_buffer)
                result = {"ok": True}
            elif action == "disturb_next_entry_selection":
                armed = {"value": True}

                def disturb_selection(widget, _param):
                    if armed["value"] and widget.get_selection_bounds() == (7, 13):
                        armed["value"] = False
                        widget.set_position(0)

                state["entry"].connect(
                    "notify::selection-bound", disturb_selection
                )
                state["entry"].connect(
                    "notify::cursor-position", disturb_selection
                )
                result = {"ok": True}
            else:
                raise ValueError(action)
        except Exception as exc:
            result = {"ok": False, "error": str(exc)}
        print(json.dumps(result, ensure_ascii=False), flush=True)
        return True

    GLib.io_add_watch(sys.stdin, GLib.IO_IN | GLib.IO_HUP, handle)
    gdk_window = window.get_window()
    xid = GdkX11.X11Window.get_xid(gdk_window)
    print(json.dumps({"ok": True, "ready": True, "xid": xid, "pid": os.getpid()}), flush=True)
    Gtk.main()
    return 0


def assert_status(actual: dict[str, object], expected: str) -> None:
    if actual.get("status") != expected:
        raise AssertionError(f"expected {expected}, got {actual}")


def test_request_identity(fixture: subprocess.Popen[str], xid: int, pid: int) -> None:
    assert command(fixture, json.dumps({"action": "setup", "target": "entry",
                                        "text": "aBc", "start": 3, "end": 3}))["ok"]
    deadline = int(time.monotonic() * 1000) + 2000
    prepare = {"protocol": 1, "action": "prepare", "operation_id": "identity-1",
               "mode": "word", "expected_fragment": "aBc", "window": xid,
               "not_after_ms": deadline}
    bound = adapter_request(pid, prepare)
    assert bound.get("ok") is True, bound
    assert adapter_request(pid, prepare) == bound
    conflict = adapter_request(pid, {**prepare, "expected_fragment": "other"})
    assert conflict.get("status") == "REJECTED", conflict
    apply = {"protocol": 1, "action": "compare_apply", "operation_id": "identity-1",
             "editor_instance": bound["editor_instance"], "mode": "word",
             "window": xid, "widget_epoch": bound["widget_epoch"],
             "context_generation": bound["context_generation"],
             "expected_revision": bound["revision"], "start": bound["start"],
             "end": bound["end"], "expected_fragment": "aBc",
             "replacement": "фИс", "not_after_ms": deadline,
             "command_identity": apply_identity(bound, "фИс", xid, deadline)}
    applied = adapter_request(pid, apply)
    assert_status(applied, "APPLIED")
    assert adapter_request(pid, prepare) == bound
    assert adapter_request(pid, apply) == applied
    assert command(fixture, json.dumps({"action": "snapshot"}))["text"] == "фИс"

    assert command(fixture, json.dumps({"action": "setup", "target": "entry",
                                        "text": "aBc", "start": 3, "end": 3}))["ok"]
    stale_prepare = {**prepare, "operation_id": "identity-stale"}
    stale_bound = adapter_request(pid, stale_prepare)
    assert stale_bound.get("ok") is True, stale_bound
    assert command(fixture, json.dumps({"action": "setup", "target": "text_view",
                                        "text": "aBc", "start": 3, "end": 3}))["ok"]
    assert adapter_request(pid, stale_prepare) == stale_bound
    stale_apply = {**apply, "operation_id": "identity-stale",
                   "editor_instance": stale_bound["editor_instance"],
                   "widget_epoch": stale_bound["widget_epoch"],
                   "context_generation": stale_bound["context_generation"],
                   "expected_revision": stale_bound["revision"],
                   "start": stale_bound["start"], "end": stale_bound["end"],
                   "command_identity": apply_identity(stale_bound, "фИс", xid, deadline)}
    assert_status(adapter_request(pid, stale_apply), "REJECTED")
    assert command(fixture, json.dumps({"action": "snapshot"}))["text"] == "aBc"
    assert command(fixture, json.dumps({"action": "setup", "target": "entry",
                                        "text": "aBc", "start": 3, "end": 3}))["ok"]
    assert_status(adapter_request(pid, stale_apply), "REJECTED")
    assert command(fixture, json.dumps({"action": "snapshot"}))["text"] == "aBc"


def test_client_request_identity(fixture: subprocess.Popen[str],
                                 client: subprocess.Popen[str]) -> None:
    assert command(fixture, json.dumps({"action": "setup", "target": "entry",
                                        "text": "aBc", "start": 3, "end": 3}))["ok"]
    first = command(client, "PREPARE_KEY client-action-1 aBc")
    assert_status(first, "PREPARED")
    assert first["operation_id"] == "client-action-1", first
    assert_status(command(client, "APPLY фИс"), "APPLIED")
    repeated = command(client, "PREPARE_KEY client-action-1 aBc")
    assert_status(repeated, "PREPARED")
    assert repeated["operation_id"] == "client-action-1", repeated
    assert_status(command(client, "APPLY фИс"), "APPLIED")
    assert command(fixture, json.dumps({"action": "snapshot"}))["text"] == "фИс"
    second = command(client, "PREPARE_KEY client-action-2 фИс")
    assert_status(second, "PREPARED")
    assert second["operation_id"] == "client-action-2", second
    assert_status(command(client, "APPLY aBc"), "APPLIED")
    assert command(fixture, json.dumps({"action": "snapshot"}))["text"] == "aBc"


def characterize_r35_race(
    fixture: subprocess.Popen[str], client: subprocess.Popen[str]
) -> None:
    """Record the old independent edit/Tab ordering; not a product GREEN."""
    assert command(fixture, json.dumps({"action": "set_entry_visibility", "visible": True}))["ok"]
    assert command(fixture, json.dumps({"action": "setup", "target": "entry", "text": "late aBc", "start": 8, "end": 8}))["ok"]
    assert_status(command(client, "PREPARE_WORD aBc"), "PREPARED")
    assert command(fixture, json.dumps({"action": "stall_next_entry_delete"}))["ok"]
    start = time.monotonic()
    client.stdin.write("APPLY фИс\n")
    client.stdin.flush()
    deadline = start + 1.0
    while time.monotonic() < deadline:
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        if observed["delete_stalled"]:
            break
        time.sleep(0.01)
    assert observed["delete_stalled"], observed
    assert observed["focused"] == "entry", observed
    stalled_ms = round((time.monotonic() - start) * 1000)
    subprocess.run(["xdotool", "key", "Tab"], check=True)
    emitted_ms = round((time.monotonic() - start) * 1000)
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        if observed["focused"] == "text_view":
            break
        time.sleep(0.01)
    assert observed["focused"] == "text_view", observed
    assert observed["entry_text"] == "late aBc", observed
    handled_ms = round((time.monotonic() - start) * 1000)
    assert_status(read_json(client, timeout=3.0), "UNKNOWN")
    unknown_ms = round((time.monotonic() - start) * 1000)
    assert command(fixture, json.dumps({"action": "release_stalled_entry_delete"}))["ok"]
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        if not observed["delete_stalled"]:
            break
        time.sleep(0.01)
    assert not observed["delete_stalled"], observed
    assert observed["entry_text"] != "late aBc", observed
    changed_ms = round((time.monotonic() - start) * 1000)
    assert stalled_ms < emitted_ms <= handled_ms < unknown_ms <= changed_ms
    print(json.dumps({
        "r35_race": "REPRODUCED_NOT_PRODUCT_GREEN",
        "tab_source": "XTEST_EXTERNAL_NOT_PUNTO_UINPUT",
        "delete_stalled_ms": stalled_ms,
        "tab_emitted_ms": emitted_ms,
        "tab_handled_ms": handled_ms,
        "client_unknown_ms": unknown_ms,
        "old_entry_changed_ms": changed_ms,
        "final_entry_text": observed["entry_text"],
        "focus_after_tab": observed["focused"],
    }, ensure_ascii=False), flush=True)


def main() -> int:
    if len(sys.argv) == 2 and sys.argv[1] == "--fixture":
        return fixture_main()
    if len(sys.argv) != 3:
        print("usage: test_gtk_editor_adapter_e2e.py DRIVER MODULE", file=sys.stderr)
        return 2
    driver = pathlib.Path(sys.argv[1]).resolve()
    module = pathlib.Path(sys.argv[2]).resolve()
    env = os.environ.copy()
    env.setdefault("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")
    env["GTK_PATH"] = str(module.parent.parent)
    env["GTK_MODULES"] = str(module)
    fixture = subprocess.Popen(
        [sys.executable, str(pathlib.Path(__file__).resolve()), "--fixture"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, bufsize=1, env=env,
    )
    client: subprocess.Popen[str] | None = None
    try:
        ready = read_json(fixture)
        assert ready.get("ready") is True, ready
        xid = int(ready["xid"])
        pid = int(ready["pid"])
        subprocess.run(["xprop", "-id", str(xid), "-f", "_NET_WM_PID", "32c", "-set", "_NET_WM_PID", str(pid)], check=True)
        subprocess.run(["xprop", "-root", "-f", "_NET_ACTIVE_WINDOW", "32x", "-set", "_NET_ACTIVE_WINDOW", hex(xid)], check=True)
        client = subprocess.Popen(
            [str(driver)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            text=True, bufsize=1, env=env,
        )
        assert_status(read_json(client), "READY")
        test_request_identity(fixture, xid, pid)
        test_client_request_identity(fixture, client)

        cases = [
            ("entry", "prefix aBc", 10, 10, "PREPARE_WORD aBc", "APPLY фИс", "prefix фИс", [10, 10]),
            ("entry", "before ghbdtn after", 7, 13, "PREPARE_SELECTION", "APPLY привет", "before привет after", [7, 13]),
            ("text_view", "prefix aBc", 10, 10, "PREPARE_WORD aBc", "APPLY фИс", "prefix фИс", [10, 10]),
            ("text_view", "before ghbdtn after", 7, 13, "PREPARE_SELECTION", "APPLY привет", "before привет after", [7, 13]),
        ]
        for target, text, start, end, prepare, apply, expected, selection in cases:
            assert command(fixture, json.dumps({"action": "setup", "target": target, "text": text, "start": start, "end": end}))["ok"]
            prepared = command(client, prepare)
            assert_status(prepared, "PREPARED")
            if prepare.startswith("PREPARE_WORD "):
                expected_fragment = prepare.removeprefix("PREPARE_WORD ")
                expected_range = [end - len(expected_fragment), end]
            else:
                expected_fragment = text[start:end]
                expected_range = [start, end]
            assert prepared["fragment"] == expected_fragment, prepared
            assert [prepared["start"], prepared["end"]] == expected_range, prepared
            assert_status(command(client, apply), "APPLIED")
            observed = command(fixture, json.dumps({"action": "snapshot"}))
            assert observed["text"] == expected, observed
            assert observed["selection"] == selection, observed
            assert observed["clipboard"] == "clipboard-sentinel", observed

        assert command(fixture, json.dumps({"action": "setup", "target": "entry", "text": "source aBc", "start": 10, "end": 10}))["ok"]
        assert_status(command(client, "PREPARE_WORD aBc"), "PREPARED")
        assert command(fixture, json.dumps({"action": "setup", "target": "entry", "text": "context B", "start": 9, "end": 9}))["ok"]
        assert_status(command(client, "APPLY фИс"), "REJECTED")
        assert command(fixture, json.dumps({"action": "snapshot"}))["text"] == "context B"

        assert command(fixture, json.dumps({"action": "setup", "target": "entry", "text": "AA ghbdtn ZZ", "start": 3, "end": 9}))["ok"]
        assert_status(command(client, "PREPARE_SELECTION"), "PREPARED")
        assert command(fixture, json.dumps({"action": "set_context", "target": "entry", "start": 0, "end": 2}))["ok"]
        assert_status(command(client, "APPLY привет"), "REJECTED")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert observed["text"] == "AA ghbdtn ZZ", observed
        assert observed["selection"] == [0, 2], observed
        assert_status(command(client, "PREPARE_SELECTION"), "PREPARED")
        assert_status(command(client, "APPLY YY"), "APPLIED")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert observed["text"] == "YY ghbdtn ZZ", observed

        assert command(fixture, json.dumps({"action": "setup", "target": "entry", "text": "focus aBc", "start": 9, "end": 9}))["ok"]
        assert_status(command(client, "PREPARE_WORD aBc"), "PREPARED")
        assert command(fixture, json.dumps({"action": "set_context", "target": "entry", "start": 0, "end": 0}))["ok"]
        assert command(fixture, json.dumps({"action": "set_context", "target": "entry", "start": 9, "end": 9}))["ok"]
        assert_status(command(client, "APPLY фИс"), "REJECTED")
        assert command(fixture, json.dumps({"action": "snapshot"}))["text"] == "focus aBc"
        assert_status(command(client, "PREPARE_WORD aBc"), "PREPARED")
        assert command(fixture, json.dumps({"action": "focus_roundtrip"}))["ok"]
        assert_status(command(client, "APPLY фИс"), "REJECTED")
        assert command(fixture, json.dumps({"action": "set_context", "target": "entry", "start": 9, "end": 9}))["ok"]
        assert_status(command(client, "PREPARE_WORD aBc"), "PREPARED")
        assert_status(command(client, "APPLY фИс"), "APPLIED")
        assert command(fixture, json.dumps({"action": "snapshot"}))["text"] == "focus фИс"

        assert command(fixture, json.dumps({"action": "setup", "target": "entry", "text": "lost aBc", "start": 8, "end": 8}))["ok"]
        assert_status(command(client, "PREPARE_WORD aBc"), "PREPARED")
        assert_status(command(client, "APPLY_LOST фИс"), "SENT")
        time.sleep(0.05)
        assert_status(command(client, "RETRY фИс"), "APPLIED")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert observed["text"] == "lost фИс", observed
        assert_status(command(client, "RETRY abc"), "REJECTED")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert observed["text"] == "lost фИс", observed

        repeated = "a" * 200 + "Z"
        assert command(fixture, json.dumps({"action": "setup", "target": "entry", "text": repeated, "start": 100, "end": 103}))["ok"]
        assert_status(command(client, "PREPARE_SELECTION"), "PREPARED")
        assert command(fixture, json.dumps({"action": "redirect_next_entry_delete", "start": 0, "end": 1}))["ok"]
        assert_status(command(client, "APPLY XYZ"), "UNKNOWN")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert observed["text"] == "a" * 199 + "Z", observed
        assert "XYZ" not in observed["text"], observed

        assert command(fixture, json.dumps({"action": "setup", "target": "text_view", "text": repeated, "start": 100, "end": 103}))["ok"]
        assert_status(command(client, "PREPARE_SELECTION"), "PREPARED")
        assert command(fixture, json.dumps({"action": "redirect_next_textview_delete", "start": 0, "end": 1}))["ok"]
        assert_status(command(client, "APPLY XYZ"), "UNKNOWN")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert observed["text"] == "a" * 199 + "Z", observed
        assert "XYZ" not in observed["text"], observed

        mutable_iters_text = "Z" + "a" * 200
        assert command(fixture, json.dumps({"action": "setup", "target": "text_view", "text": mutable_iters_text, "start": 100, "end": 103}))["ok"]
        assert_status(command(client, "PREPARE_SELECTION"), "PREPARED")
        assert command(fixture, json.dumps({"action": "mutate_next_textview_delete_iters", "start": 0, "end": 3}))["ok"]
        assert_status(command(client, "APPLY XYZ"), "UNKNOWN")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert "XYZ" not in observed["text"], observed

        assert command(fixture, json.dumps({
            "action": "replace_textview_with_pre_delete_mutator",
            "text": mutable_iters_text,
            "selection_start": 100,
            "selection_end": 103,
            "start": 0,
            "end": 3,
        }))["ok"]
        assert_status(command(client, "PREPARE_SELECTION"), "PREPARED")
        assert_status(command(client, "APPLY XYZ"), "UNKNOWN")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert "XYZ" not in observed["text"], observed

        assert command(fixture, json.dumps({"action": "setup", "target": "text_view", "text": mutable_iters_text, "start": 100, "end": 103}))["ok"]
        assert_status(command(client, "PREPARE_SELECTION"), "PREPARED")
        assert_status(command(client, "APPLY XYZ"), "APPLIED")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert observed["text"] == "Z" + "a" * 99 + "XYZ" + "a" * 98, observed

        assert command(fixture, json.dumps({"action": "setup", "target": "text_view", "text": mutable_iters_text, "start": 100, "end": 103}))["ok"]
        assert_status(command(client, "PREPARE_SELECTION"), "PREPARED")
        assert command(fixture, json.dumps({"action": "mutate_next_textview_insert_iter", "offset": 0}))["ok"]
        assert_status(command(client, "APPLY XYZ"), "UNKNOWN")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert observed["text"] == "XYZZ" + "a" * 197, observed

        assert command(fixture, json.dumps({
            "action": "replace_textview_with_pre_insert_mutator",
            "text": mutable_iters_text,
            "selection_start": 100,
            "selection_end": 103,
            "offset": 0,
        }))["ok"]
        assert_status(command(client, "PREPARE_SELECTION"), "PREPARED")
        assert_status(command(client, "APPLY XYZ"), "UNKNOWN")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert observed["text"] == "XYZZ" + "a" * 197, observed

        assert command(fixture, json.dumps({"action": "setup", "target": "text_view", "text": mutable_iters_text, "start": 100, "end": 103}))["ok"]
        assert_status(command(client, "PREPARE_SELECTION"), "PREPARED")
        assert_status(command(client, "APPLY XYZ"), "APPLIED")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert observed["text"] == "Z" + "a" * 99 + "XYZ" + "a" * 98, observed

        assert command(fixture, json.dumps({"action": "setup", "target": "entry", "text": "outside aBc", "start": 11, "end": 11}))["ok"]
        assert_status(command(client, "PREPARE_WORD aBc"), "PREPARED")
        assert command(fixture, json.dumps({"action": "insert_on_next_entry_delete"}))["ok"]
        assert_status(command(client, "APPLY фИс"), "UNKNOWN")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert "фИс" not in observed["text"], observed

        assert command(fixture, json.dumps({"action": "setup", "target": "entry", "text": "ttl aBc", "start": 7, "end": 7}))["ok"]
        assert_status(command(client, "PREPARE_SHORT aBc"), "PREPARED")
        assert_status(command(client, "APPLY_LOST фИс"), "SENT")
        time.sleep(1.1)
        assert_status(command(client, "RETRY фИс"), "APPLIED")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert observed["text"] == "ttl фИс", observed
        assert_status(command(client, "RETRY_UNKNOWN фИс"), "UNKNOWN")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert observed["text"] == "ttl фИс", observed

        assert command(fixture, json.dumps({"action": "setup", "target": "entry", "text": "pre aBc post", "start": 7, "end": 7}))["ok"]
        assert_status(command(client, "PREPARE_WORD aBc"), "PREPARED")
        assert command(fixture, json.dumps({"action": "block_next_entry_delete"}))["ok"]
        assert_status(command(client, "APPLY фИс"), "UNKNOWN")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert observed["text"] == "pre aBc post", observed
        assert_status(command(client, "PREPARE_WORD aBc"), "PREPARED")
        assert_status(command(client, "APPLY фИс"), "APPLIED")
        assert command(fixture, json.dumps({"action": "snapshot"}))["text"] == "pre фИс post"

        assert command(fixture, json.dumps({"action": "setup", "target": "text_view", "text": "pre aBc post", "start": 7, "end": 7}))["ok"]
        assert_status(command(client, "PREPARE_WORD aBc"), "PREPARED")
        assert command(fixture, json.dumps({"action": "replace_buffer_on_delete", "text": "pre aBc post", "start": 7, "end": 7}))["ok"]
        assert_status(command(client, "APPLY фИс"), "UNKNOWN")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert observed["text"] == "pre aBc post", observed
        assert_status(command(client, "PREPARE_WORD aBc"), "PREPARED")
        assert_status(command(client, "APPLY фИс"), "APPLIED")
        assert command(fixture, json.dumps({"action": "snapshot"}))["text"] == "pre фИс post"

        assert command(fixture, json.dumps({"action": "setup", "target": "entry", "text": "before ghbdtn after", "start": 7, "end": 13}))["ok"]
        assert_status(command(client, "PREPARE_SELECTION"), "PREPARED")
        assert command(fixture, json.dumps({"action": "disturb_next_entry_selection"}))["ok"]
        assert_status(command(client, "APPLY привет"), "UNKNOWN")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert observed["text"] == "before привет after", observed
        assert observed["selection"] == [0, 0], observed
        assert command(fixture, json.dumps({"action": "set_context", "target": "entry", "start": 7, "end": 13}))["ok"]
        assert_status(command(client, "PREPARE_SELECTION"), "PREPARED")
        assert_status(command(client, "APPLY ghbdtn"), "APPLIED")

        assert command(fixture, json.dumps({"action": "setup", "target": "entry", "text": "old aBc", "start": 7, "end": 7}))["ok"]
        assert_status(command(client, "PREPARE_WORD aBc"), "PREPARED")
        assert command(fixture, json.dumps({"action": "recreate_entry", "text": "old aBc"}))["ok"]
        assert_status(command(client, "APPLY фИс"), "REJECTED")
        assert_status(command(client, "PREPARE_WORD aBc"), "PREPARED")
        assert_status(command(client, "APPLY фИс"), "APPLIED")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert observed["text"] == "old фИс", observed
        assert observed["clipboard"] == "clipboard-sentinel", observed

        assert command(fixture, json.dumps({"action": "setup", "target": "text_view", "text": "document aBc", "start": 12, "end": 12}))["ok"]
        assert_status(command(client, "PREPARE_WORD aBc"), "PREPARED")
        assert command(fixture, json.dumps({"action": "replace_text_buffer", "text": "document aBc"}))["ok"]
        assert_status(command(client, "APPLY фИс"), "REJECTED")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert observed["text"] == "document aBc", observed
        assert_status(command(client, "PREPARE_WORD aBc"), "PREPARED")
        assert_status(command(client, "APPLY фИс"), "APPLIED")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert observed["text"] == "document фИс", observed

        assert command(fixture, json.dumps({"action": "setup", "target": "entry", "text": "late aBc", "start": 8, "end": 8}))["ok"]
        assert_status(command(client, "PREPARE_SHORT aBc"), "PREPARED")
        time.sleep(1.1)
        assert_status(command(client, "APPLY фИс"), "REJECTED")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert observed["text"] == "late aBc", observed

        assert_status(command(client, "PREPARE_WORD aBc"), "PREPARED")
        assert_status(command(client, "APPLY фИс"), "APPLIED")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert observed["text"] == "late фИс", observed

        assert command(fixture, json.dumps({"action": "setup", "target": "entry", "text": "secret", "start": 6, "end": 6}))["ok"]
        assert command(fixture, json.dumps({"action": "set_entry_visibility", "visible": False}))["ok"]
        assert_status(command(client, "PREPARE_WORD secret"), "REJECTED")
        observed = command(fixture, json.dumps({"action": "snapshot"}))
        assert observed["text"] == "secret", observed

        if os.environ.get("PUNTO_CHARACTERIZE_R35_RACE") == "1":
            characterize_r35_race(fixture, client)
        if os.environ.get("PUNTO_TEST_GDK_TAB_GATE") == "1":
            test_gdk_tab_gate(fixture, xid, pid)
        print("test_gtk_editor_adapter_e2e: OK")
        return 0
    finally:
        if client is not None:
            client.terminate()
            client.wait(timeout=3)
        fixture.terminate()
        try:
            fixture.wait(timeout=3)
        except subprocess.TimeoutExpired:
            fixture.kill()
            fixture.wait(timeout=3)


if __name__ == "__main__":
    raise SystemExit(main())
