"""Dry safety controls; these tests are not installed-product acceptance."""

import json
import ctypes as C
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, call, patch

from installed_guest import Collector, Failure, Guard, Guest, Lines, checked_run, check_files, guest_group, kill_guest_group, ordinary_environment, qmp_keys, verify_state, desktop_receipt, verify_desktop


from installed_guest import (PIPELINE_ROLES, LIFECYCLE_FILES, canonical_sound_pair, public_stats,
                             successor_identity, durable_learning, validate_audio_player, validate_audio_window)


class InstalledGuestControls(unittest.TestCase):
    def exact285_manifest(self):
        return {"profile": "exact285-gtk16", "package": {"version": "2.8.5", "sha256":
                "80c977e4b195f73bdbf89c9e2ba8b1c4162a6c318ea74142fedd6010d222ebd4"},
                "installed": {"/usr/local/bin/punto-daemon": "edc37fc25bd16d79e8a1252d1e8a9ef012099b271f8c8b6e7f17176d173cd6a6",
                    "/usr/local/bin/punto": "07b56664729be21303560f7912028e0c64e056b375573dcfd21d92875e3b4579",
                    "/usr/local/bin/punto-tray": "45881738bda0433702bcbe2bc211e07f343f109360a2a75840a43676cddf5db1"},
                "processes": {"daemon": "/usr/local/bin/punto-daemon", "udevmon": "/usr/bin/udevmon",
                    "interception": "/usr/bin/intercept", "uinput": "/usr/bin/uinput",
                    "gnome": "/usr/bin/gnome-shell", "ibus": "/usr/bin/ibus-daemon"}}

    def test_exact285_explicit_admission_and_manifest_negatives(self):
        import copy
        import installed_guest
        admission = getattr(installed_guest, "profile_contract", None)
        self.assertTrue(callable(admission), "explicit archive-pinned profile admission is absent")
        self.assertTrue(admission(self.exact285_manifest()))
        self.assertFalse(admission({}))
        for mutate in (lambda m: m.update(profile="unknown"),
                lambda m: m["package"].update(version="2.8.14"),
                lambda m: m["package"].update(sha256="0" * 64),
                lambda m: m["installed"].update({"/usr/local/bin/punto-daemon": "0" * 64}),
                lambda m: m["processes"].update(daemon="/usr/bin/punto-daemon"),
                lambda m: m["processes"].update(helper="/usr/libexec/punto-session-bridge")):
            manifest = copy.deepcopy(self.exact285_manifest())
            mutate(manifest)
            with self.subTest(manifest=manifest), self.assertRaises(Failure):
                admission(manifest)
        for flag in ("features", "terminal_phase", "lifecycle_phase", "desktop_parity", "soak"):
            manifest = self.exact285_manifest()
            manifest[flag] = True
            with self.subTest(flag=flag), self.assertRaises(Failure):
                admission(manifest)

    def test_exact285_status_protocol_is_explicit_and_readback_pinned(self):
        import struct
        from installed_guest import runtime_status
        fields = {"profile": "exact285-gtk16"}
        def client(reply, pid=42, uid=0):
            value = Mock()
            value.recv.return_value = reply
            value.getsockopt.return_value = struct.pack("3i", pid, uid, 0)
            return Mock(__enter__=Mock(return_value=value), __exit__=Mock(return_value=False)), value
        for enabled in (False, True):
            reply = b"OK ENABLED\n" if enabled else b"OK DISABLED\n"
            first, sender = client(reply)
            second, reader = client(reply)
            with self.subTest(enabled=enabled), patch("installed_guest.socket.socket", side_effect=[first, second]):
                try:
                    self.assertIs(runtime_status(enabled, expected_pid=42, **fields), enabled)
                except Failure as error:
                    self.fail(f"legacy typed status refused: {error}")
                sender.sendall.assert_called_once_with(f"SET_STATUS {int(enabled)}\n".encode())
                reader.sendall.assert_called_once_with(b"GET_STATUS\n")
        for reply in (b"OK: ENABLED\n", b"OK: DISABLED\n", b"OK Scheduled\n", b"OK ENABLED\nextra", b"ERROR: denied\n"):
            context, _ = client(reply)
            with self.subTest(reply=reply), patch("installed_guest.socket.socket", return_value=context), self.assertRaises(Failure):
                runtime_status(expected_pid=42, **fields)
        for pid, uid in ((99, 0), (42, 1000)):
            context, sender = client(b"OK ENABLED\n", pid, uid)
            with patch("installed_guest.socket.socket", return_value=context), self.assertRaises(Failure):
                runtime_status(True, expected_pid=42, **fields)
            sender.sendall.assert_not_called()
        first, _ = client(b"OK ENABLED\n")
        second, _ = client(b"OK DISABLED\n")
        with patch("installed_guest.socket.socket", side_effect=[first, second]), self.assertRaises(Failure):
            runtime_status(True, expected_pid=42, **fields)

    def test_default_status_never_accepts_invented_colon_wire(self):
        from installed_guest import runtime_status
        for reply in (b"OK: ENABLED\n", b"OK: DISABLED\n"):
            client = Mock(**{"recv.return_value": reply})
            context = Mock(__enter__=Mock(return_value=client), __exit__=Mock(return_value=False))
            with patch("installed_guest.socket.socket", return_value=context), self.assertRaises(Failure):
                runtime_status()

    def test_exact285_status_requires_positive_integer_peer_before_connect(self):
        from installed_guest import runtime_status
        for pid in (None, 0, -1, True, "42"):
            with self.subTest(pid=pid), patch("installed_guest.socket.socket") as connect, self.assertRaises(Failure):
                runtime_status(expected_pid=pid, profile="exact285-gtk16")
            connect.assert_not_called()

    def test_exact285_setup_never_launches_chromium(self):
        guest = Guest.__new__(Guest)
        guest.m = {**self.exact285_manifest(), "staged_dir": "/owned", "chromium": "/usr/bin/chromium", "run_id": "owned"}
        with patch.dict("installed_guest.os.environ", {"XDG_SESSION_TYPE": "x11"}, clear=True), \
                patch("installed_guest.Lines", return_value=Mock(**{"read.return_value": {"ready": True}})) as lines, \
                patch.dict(sys.modules, {"playwright": Mock(), "playwright.sync_api": Mock()}), \
                patch("installed_guest.subprocess.Popen", side_effect=AssertionError("legacy launched Chromium")) as spawn:
            guest.setup()
        self.assertEqual(lines.call_count, 2)
        spawn.assert_not_called()

    def test_exact285_forbids_modern_actions_before_projection(self):
        for surface, action in (("chromium", "prepare"), ("terminal", "start"), ("gtk", "stats"),
                                ("gtk", "lifecycle-begin"), ("gtk", "clipboard")):
            guest = Guest.__new__(Guest)
            guest.m = self.exact285_manifest()
            guest.prepare, guest.system, guest.target = Mock(return_value={}), Mock(return_value={}), Mock(return_value={})
            guest.terminal_start, guest.lifecycle_begin = Mock(return_value={}), Mock(return_value={})
            with self.subTest(surface=surface, action=action), self.assertRaises(Failure):
                guest.call({"surface": surface, "action": action, "request_id": "negative", "nonce": "owned"})
            with self.subTest(no_forwarding=(surface, action)):
                for owner in (guest.prepare, guest.system, guest.target, guest.terminal_start, guest.lifecycle_begin):
                    owner.assert_not_called()

    def test_exact285_cohort_exact_sixteen_unique_manual_tuples(self):
        collector = Collector(self.exact285_manifest(), Mock(), None, None)
        cohort = getattr(collector, "legacy", None)
        self.assertTrue(callable(cohort), "finite exact285 cohort is absent")
        collector.rpc = Mock(side_effect=[{"enabled": True}, {"enabled": False}, {"enabled": True}])
        collector.check = Mock(return_value={"group": 1})
        seen = []
        def operation(surface, direction=None, mode="word", widget=None):
            seen.append((surface, widget, direction, mode))
            collector.last_group = 1 - direction
            collector.completed += 1
        collector.operation = Mock(side_effect=operation)
        cohort()
        expected = {("gtk", widget, direction, mode) for widget in ("entry", "text_view")
                    for direction in (0, 1) for mode in ("word", "selection", "empty", "repeat")}
        self.assertEqual(set(seen), expected)
        self.assertEqual(len(seen), len(expected))
        self.assertEqual(collector.completed, 16)
        self.assertTrue(collector.original_status)
        self.assertEqual([call.kwargs for call in collector.rpc.call_args_list], [{}, {"enabled": False}, {"enabled": True}])

    def test_exact285_cohort_cleanup_retains_safety_priority(self):
        for failure in (Failure("product refusal", "FAIL_PRODUCT"), Failure("core changed", "ABORTED_SAFETY")):
            collector = Collector(self.exact285_manifest(), Mock(), None, None)
            cohort = getattr(collector, "legacy", None)
            self.assertTrue(callable(cohort), "finite exact285 cohort is absent")
            collector.check = Mock(return_value={"group": 0})
            collector.operation = Mock(side_effect=failure)
            collector.rpc = Mock(side_effect=[{"enabled": True}, {"enabled": False}, Failure("restore core changed", "ABORTED_SAFETY")])
            with self.subTest(primary=failure.kind), self.assertRaises(Failure) as raised:
                cohort()
            self.assertEqual(raised.exception.kind, "ABORTED_SAFETY")
            self.assertIs(raised.exception.__cause__, failure)

    def test_exact285_operation_keeps_explicit_widget_and_unique_case(self):
        import inspect
        self.assertIn("widget", inspect.signature(Collector.operation).parameters)
        for widget in ("entry", "text_view"):
            before = self.state("", 0)
            before["state"]["selection"] = [0, 0]
            with tempfile.TemporaryDirectory() as directory:
                collector = Collector(self.exact285_manifest(), None, None, Path(directory))
                collector.rpc = Mock(return_value=before)
                collector.authority, collector.key, collector.wait_state = Mock(), Mock(), Mock()
                collector.arm = Mock(return_value=("owned-attempt", before))
                collector.operation("gtk", 0, "selection", widget=widget)
                self.assertTrue(all(call.kwargs.get("target") == widget for call in collector.rpc.call_args_list
                                    if call.args[1] in ("prepare", "complete")))
                self.assertTrue(all(call.args[2] == widget for call in collector.arm.call_args_list))
                self.assertTrue(all(call.args[1] == widget for call in collector.wait_state.call_args_list))
                record = json.loads((Path(directory) / "operations.jsonl").read_text())
                self.assertEqual(record["case_id"], f"gtk-{widget}-0-selection")

    def test_exact285_actual_system_rejects_executable_writer_before_other_observers(self):
        for executable in ("/usr/bin/punto-daemon", "/usr/bin/punto-daemon (deleted)",
                           "/usr/bin/punto-tray", "/usr/bin/punto-tray (deleted)",
                           "/usr/libexec/punto-session-bridge", "/owned/punto-session-bridge (deleted)"):
            guest = Guest.__new__(Guest)
            guest.m = self.exact285_manifest()
            with self.subTest(executable=executable), \
                    patch("installed_guest.checked_run", return_value="/usr/local/bin/punto-daemon\n" + executable + "\n") as observe, \
                    patch("installed_guest.check_files") as files, self.assertRaises(Failure) as raised:
                guest.system()
            self.assertEqual(raised.exception.kind, "ABORTED_SAFETY")
            self.assertEqual(observe.call_args.args[0], ["sudo", "-n", "ps", "-e", "-o", "exe=", "-ww"])
            files.assert_not_called()

    def test_exact285_executable_projection_admits_kernel_threads_and_legacy_paths(self):
        from installed_guest import reject_candidate_writers
        command = ["sudo", "-n", "ps", "-e", "-o", "exe=", "-ww"]
        def projection(argv):
            if argv != command:
                raise Failure("obsolete executable projection failed", "BLOCKED_PRECONDITION")
            return "-\n/usr/local/bin/punto-daemon\n/usr/local/bin/punto-tray\n/usr/bin/python3\n"
        with patch("installed_guest.checked_run", side_effect=projection) as observe:
            reject_candidate_writers()
        observe.assert_called_once_with(command)

    def test_exact285_executable_projection_fails_closed_on_missing_daemon_or_command_fault(self):
        from installed_guest import reject_candidate_writers
        with patch("installed_guest.checked_run", return_value="-\n/usr/local/bin/punto-tray\n"), self.assertRaises(Failure):
            reject_candidate_writers()
        fault = Failure("procps failed", "BLOCKED_PRECONDITION")
        with patch("installed_guest.checked_run", side_effect=fault), self.assertRaises(Failure) as raised:
            reject_candidate_writers()
        self.assertIs(raised.exception, fault)

    def test_exact285_status_shared_deadline_never_sends_late_readback(self):
        import struct
        from installed_guest import runtime_status
        client = Mock(**{"recv.return_value": b"OK ENABLED\n"})
        client.getsockopt.return_value = struct.pack("3i", 42, 0, 0)
        context = Mock(__enter__=Mock(return_value=client), __exit__=Mock(return_value=False))
        with patch("installed_guest.socket.socket", return_value=context), \
                patch("installed_guest.time.monotonic", side_effect=[0, .1, .1, .2, .3, .4, .5, 1.01]), \
                self.assertRaises(Failure):
            runtime_status(True, expected_pid=42, profile="exact285-gtk16")
        client.sendall.assert_called_once_with(b"SET_STATUS 1\n")

    def test_exact285_connect_budget_shrinks_send_timeout(self):
        import struct
        from installed_guest import runtime_status
        clock, send_timeout = [0.0], []
        client = Mock(**{"recv.return_value": b"OK ENABLED\n"})
        client.getsockopt.return_value = struct.pack("3i", 42, 0, 0)
        client.connect.side_effect = lambda _: clock.__setitem__(0, .9)
        client.sendall.side_effect = lambda _: send_timeout.append(client.settimeout.call_args.args[0])
        context = Mock(__enter__=Mock(return_value=client), __exit__=Mock(return_value=False))
        with patch("installed_guest.socket.socket", return_value=context), \
                patch("installed_guest.time.monotonic", side_effect=lambda: clock[0]):
            self.assertTrue(runtime_status(expected_pid=42, profile="exact285-gtk16"))
        self.assertEqual(len(send_timeout), 1)
        self.assertAlmostEqual(send_timeout[0], .1)

    def test_exact285_actual_arm_keeps_widget_on_arm_and_snapshot(self):
        identity = {"observer_id": "gtk", "observer_instance_id": "real-instance", "observer_ns": 100,
                    "host_interval_ns": [200, 240]}
        collector = Collector({**self.exact285_manifest(), "run_id": "owned"}, None, None, None)
        collector.rpc = Mock(side_effect=[identity, {}, self.state("", 0)])
        collector.arm("gtk", "gtk-text_view-0-word", "text_view")
        self.assertEqual(collector.rpc.call_args_list[1].kwargs["target"], "text_view")
        self.assertEqual(collector.rpc.call_args_list[2].kwargs["target"], "text_view")

    def test_exact285_guest_status_projects_only_pinned_profile_and_authority(self):
        guest = Guest.__new__(Guest)
        guest.m = self.exact285_manifest()
        before = {"identity": {"daemon": [42, "123"]}, "clipboard": "owned", "pressed": []}
        guest.system = Mock(return_value=before)
        with patch("installed_guest.runtime_status", return_value=False) as status:
            result = guest.call({"surface": "gtk", "action": "status", "enabled": False, "request_id": "owned"})
        self.assertFalse(result["enabled"])
        status.assert_called_once_with(False, expected_pid=42, profile="exact285-gtk16")
        guest.system = Mock(side_effect=[before, {**before, "identity": {"daemon": [43, "124"]}}])
        with patch("installed_guest.runtime_status", return_value=False), self.assertRaises(Failure) as raised:
            guest.call({"surface": "gtk", "action": "status", "enabled": False, "request_id": "drift"})
        self.assertEqual(raised.exception.kind, "ABORTED_SAFETY")

    def test_exact285_independent_restore_uses_same_profile_and_live_identity(self):
        import io
        from installed_guest import restore_status_main
        manifest = self.exact285_manifest()
        manifest.update(domain_uuid="owned-domain", observer_assets={})
        request = {"manifest": manifest, "enabled": False, "daemon": [42, "123"]}
        stat_line = "42 (daemon) " + " ".join(["S"] + ["0"] * 18 + ["123"])
        def observe(argv, **_):
            if argv[:3] == ["sudo", "-n", "cat"]:
                return "owned-domain\n"
            if argv[:3] == ["sudo", "-n", "readlink"]:
                return "/usr/local/bin/punto-daemon\n"
            if argv[:3] == ["sudo", "-n", "sha256sum"]:
                return manifest["installed"]["/usr/local/bin/punto-daemon"] + "  /proc/42/exe\n"
            if argv[0] == "dpkg-query":
                return "ii  2.8.5\n"
            raise AssertionError("unexpected recovery command")
        with patch("installed_guest.sys.stdin", io.StringIO(json.dumps(request) + "\n")), \
                patch("installed_guest.checked_run", side_effect=observe), patch("installed_guest.check_files"), \
                patch("installed_guest.Path.read_text", return_value=stat_line), \
                patch("installed_guest.reject_candidate_writers") as writers, \
                patch("installed_guest.runtime_status", return_value=False) as status, patch("builtins.print") as output:
            restore_status_main()
        writers.assert_called_once_with()
        status.assert_called_once_with(False, expected_pid=42, profile="exact285-gtk16")
        self.assertEqual(json.loads(output.call_args.args[0]), {"enabled": False})
        with patch("installed_guest.sys.stdin", io.StringIO(json.dumps(request) + "\n")), \
                patch("installed_guest.checked_run", side_effect=observe), patch("installed_guest.check_files"), \
                patch("installed_guest.Path.read_text", return_value=stat_line.replace("123", "124")), \
                patch("installed_guest.runtime_status") as status, self.assertRaises(Failure):
            restore_status_main()
        status.assert_not_called()

    def gtk_preparation(self, *, realized=True, stamp=1234, armed=False):
        import gi
        gi.require_version("GdkX11", "3.0")
        from gi.repository import GdkX11
        import gtk_target
        from installed_guest import gtk_main
        instances, ordering = [], []
        native = Mock()
        native.get_events.return_value = gtk_target.Gdk.EventMask.KEY_PRESS_MASK
        native.set_events.side_effect = lambda mask: ordering.append(("mask", int(mask)))
        class Scratch(gtk_target.ScratchTarget):
            def __init__(self):
                from probe import ObservationProtocol
                self.protocol = ObservationProtocol("gtk")
                if armed:
                    self.protocol.run_id = self.protocol.case_id = self.protocol.attempt_id = "active"
                self.window = Mock()
                self.window.get_window.return_value = native if realized else None
                self.window.present_with_time.side_effect = lambda value: ordering.append(("present", value))
                self.entry = Mock()
                instances.append(self)
        def clock(window):
            self.assertIs(window, native)
            ordering.append(("clock",))
            return stamp
        with patch("gtk_target.ScratchTarget", Scratch), \
                patch("gtk_target.GLib.io_add_watch"), patch("gtk_target.Gtk.main"), \
                patch.object(GdkX11, "x11_get_server_time", side_effect=clock), \
                patch("builtins.print") as output:
            gtk_main()
            target = instances[0]
            target.handle_line(json.dumps({"action": "prepare", "target": "entry", "text": ""}))
            result = json.loads(output.call_args.args[0])
        return target, native, ordering, result

    def test_gtk_preparation_uses_current_server_time_after_event_mask(self):
        import gtk_target
        target, native, ordering, result = self.gtk_preparation()
        self.assertTrue(result["ok"])
        self.assertEqual(ordering, [
            ("mask", int(gtk_target.Gdk.EventMask.KEY_PRESS_MASK | gtk_target.Gdk.EventMask.PROPERTY_CHANGE_MASK)),
            ("clock",), ("present", 1234),
        ])
        target.window.present.assert_not_called()
        native.get_events.assert_called_once()

    def test_gtk_preparation_rejects_unrealized_zero_time_and_armed_target(self):
        for options in ({"realized": False}, {"stamp": 0}, {"armed": True}):
            with self.subTest(options=options):
                target, _native, ordering, result = self.gtk_preparation(**options)
                self.assertFalse(result["ok"])
                self.assertNotIn("present", [item[0] for item in ordering])
                target.window.present.assert_not_called()

    def test_setup_browser_uses_basic_password_store_in_isolated_profile(self):
        import os
        class SpawnCaptured(Exception):
            pass
        guest = Guest.__new__(Guest)
        guest.m = {"staged_dir": "/isolated/punto-guest-test", "chromium": "/usr/bin/chromium",
                   "run_id": "scratch-run-nonce"}
        api = Mock()
        with patch.dict("installed_guest.os.environ", {"XDG_SESSION_TYPE": "x11"}, clear=True), \
                patch("installed_guest.Lines", return_value=Mock(**{"read.return_value": {"ready": True}})), \
                patch.dict(sys.modules, {"playwright": Mock(), "playwright.sync_api": api}), \
                patch("installed_guest.subprocess.Popen", side_effect=SpawnCaptured) as spawn:
            with self.assertRaises(SpawnCaptured):
                guest.setup()
            self.assertEqual(dict(os.environ), {"XDG_SESSION_TYPE": "x11"})
        spawn.assert_called_once()
        argv = spawn.call_args.args[0]
        self.assertEqual(argv[:4], [sys.executable, str(Path(__file__).parent / "installed_guest.py"),
                                  "--exec", guest.m["chromium"]])
        self.assertEqual([arg for arg in argv if arg.startswith("--user-data-dir=")],
                         ["--user-data-dir=/isolated/punto-guest-test/chrome-profile"])
        self.assertEqual([arg for arg in argv if arg.startswith("--app=")],
                         ["--app=" + (Path(__file__).parent / "chromium_target.html").as_uri() + "?run=scratch-run-nonce"])
        self.assertFalse(any(arg.startswith(("--force-renderer-accessibility", "--enable-accessibility")) for arg in argv))
        self.assertNotIn("env", spawn.call_args.kwargs)
        self.assertTrue(spawn.call_args.kwargs["start_new_session"])
        self.assertIn("--password-store=basic", argv)

    def test_gtk_target_fresh_import_selects_gtk3_and_gdk3(self):
        result = subprocess.run(
            [sys.executable, "-c", "import json; import gtk_target; "
             "print(json.dumps([gtk_target.Gtk._version, gtk_target.Gdk._version]))"],
            cwd=Path(__file__).parent, capture_output=True, text=True, timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), ["3.0", "3.0"])

    def test_feature_chords_remain_fixed_and_ordered(self):
        for chord in (["tab"], ["ctrl", "pause"], ["alt", "pause"],
                      ["ctrl", "alt", "pause"], ["ctrl", "z"], ["shift", "h"]):
            events = json.loads(qmp_keys(chord))["arguments"]["events"]
            self.assertEqual([e["data"]["down"] for e in events],
                             [True] * len(chord) + [False] * len(chord))
        for chord in (["alt", "f"], ["ctrl", "alt", "delete"], ["ctrl", "shift", "a"]):
            with self.assertRaises(Failure):
                qmp_keys(chord)

    def tab_states(self):
        from copy import deepcopy
        before = self.state("ghbdtn", 0)
        before["state"].update(focus_target="gtk_entry", following_text="", keys={"tab_down": 0, "tab_up": 0})
        before["clipboard"] = {"owner": 42, "targets": "TARGETS\nUTF8_STRING\n",
                               "bytes_base64": "YQ==", "representations": {"UTF8_STRING": ["UTF8_STRING", 8, "YQ=="]}}
        after = deepcopy(before)
        after["state"].update(text="привет", focus_target="gtk_text_view", keys={"tab_down": 1, "tab_up": 1})
        after["group"] = 1
        after["clipboard"]["owner"] = 43
        return before, after

    def test_tab_restore_is_explicit_and_direct_replay_owner_stays_strict(self):
        before, after = self.tab_states()
        with self.assertRaises(Failure):
            verify_state(before, after, "привет", [6, 6], 1)
        verify_state(before, after, "привет", [6, 6], 1, expected_focus="gtk_text_view",
                     expected_following="", clipboard_payload_restore=True)

    def test_proved_paste_restoration_allows_only_service_metadata_change(self):
        before, after = self.tab_states()
        after["clipboard"]["targets"] = "TIMESTAMP\nTARGETS\nMULTIPLE\nSAVE_TARGETS\nUTF8_STRING\n"
        verify_state(before, after, "привет", [6, 6], 1, expected_focus="gtk_text_view",
                     expected_following="", clipboard_payload_restore=True)
        with self.assertRaises(Failure):
            verify_state(before, after, "привет", [6, 6], 1,
                         expected_focus="gtk_text_view", expected_following="")

    def test_tab_restore_never_accepts_unknown_representation_or_wrong_field(self):
        from copy import deepcopy
        before, after = self.tab_states()
        for mutation in ("bytes", "type", "format", "targets", "owner", "focus", "foreign", "identity"):
            changed = deepcopy(after)
            if mutation in ("bytes", "type", "format"):
                index = {"type": 0, "format": 1, "bytes": 2}[mutation]
                changed["clipboard"]["representations"]["UTF8_STRING"][index] = "wrong"
            elif mutation == "targets":
                changed["clipboard"]["targets"] += "text/html\n"
            elif mutation == "owner":
                changed["clipboard"]["owner"] = 0
            elif mutation == "focus":
                changed["state"]["focus_target"] = "none"
            elif mutation == "foreign":
                changed["state"]["following_text"] = "leaked"
            else:
                changed["identity"] = {"daemon": [43, "456"]}
            with self.subTest(mutation=mutation), self.assertRaises(Failure):
                verify_state(before, changed, "привет", [6, 6], 1, expected_focus="gtk_text_view",
                             expected_following="", clipboard_payload_restore=True)
        del before["clipboard"]["representations"]
        with self.assertRaises(Failure):
            verify_state(before, after, "привет", [6, 6], 1, expected_focus="gtk_text_view",
                         expected_following="", clipboard_payload_restore=True)

    def test_native_key_oracle_rejects_duplicate_or_incomplete_lifecycle(self):
        before, after = self.tab_states()
        Collector.require_key_count(before, after, "tab", 1)
        for down, up in ((2, 2), (1, 0), (0, 1)):
            after["state"]["keys"] = {"tab_down": down, "tab_up": up}
            with self.assertRaises(Failure):
                Collector.require_key_count(before, after, "tab", 1)

    def test_undo_deadline_checked_after_authority_before_send(self):
        guard = Mock()
        with tempfile.TemporaryDirectory() as directory:
            package = Path(directory) / "package"
            package.write_bytes(b"")
            manifest = {"sources": {}, "package": {"path": str(package), "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}}
            collector = Collector(manifest, guard, None, Path(directory))
            collector.check = Mock()
            with patch("installed_guest.time.monotonic_ns", return_value=2_500_000_001), self.assertRaises(Failure):
                collector.key("gtk", "ctrl", "z", deadline_ns=2_500_000_000)
            collector.check.assert_called_once()
            guard.send.assert_not_called()

    def test_undo_guard_rechecks_deadline_after_domain_queries(self):
        guard = Guard("punto-r23-native", "exact-uuid", run=Mock())
        guard.check = Mock()
        with patch("installed_guest.time.monotonic_ns", return_value=3), self.assertRaises(Failure):
            guard.send(["ctrl", "z"], deadline_ns=2)
        guard.check.assert_called_once()
        guard.run.assert_not_called()

    def test_status_feature_restores_original_after_product_failure(self):
        collector = Collector({}, Mock(), None, None)
        collector.original_status = None
        collector.rpc = Mock(return_value={"enabled": False})
        collector.feature_edit = Mock(side_effect=Failure("real product refusal", "FAIL_PRODUCT"))
        collector.feature_tab = Mock()
        collector.feature_undo = Mock()
        collector.feature_status = Mock()
        with self.assertRaises(Failure):
            collector.features()
        statuses = [call.kwargs for call in collector.rpc.call_args_list if call.args[1] == "status"]
        self.assertEqual(statuses[0], {})
        self.assertEqual(statuses[-1], {"enabled": False})
        self.assertFalse(collector.original_status)

    def test_status_restore_eof_preserves_primary_product_failure(self):
        collector = Collector({}, Mock(), None, None)
        primary = Failure("primary product refusal", "FAIL_PRODUCT")
        collector.rpc = Mock(side_effect=[{"enabled": True}, {"enabled": False}, Failure("observer EOF")])
        collector.feature_edit = Mock(side_effect=primary)
        collector.feature_tab = Mock()
        collector.feature_undo = Mock()
        collector.feature_status = Mock()
        with self.assertRaises(Failure) as raised:
            collector.features()
        self.assertIs(raised.exception, primary)
        self.assertEqual(raised.exception.kind, "FAIL_PRODUCT")
        self.assertEqual(raised.exception.__notes__,
                         ["Runtime status restoration failed after the original feature failure."])
        self.assertEqual(collector.rpc.call_args.args, ("chromium", "status"))
        self.assertEqual(collector.rpc.call_args.kwargs, {"enabled": True})

    def test_status_restore_eof_without_primary_remains_failure(self):
        collector = Collector({}, Mock(), None, None)
        restoration = Failure("observer EOF")
        collector.rpc = Mock(return_value={"enabled": True})
        collector.feature_edit = Mock()
        collector.feature_tab = Mock()
        collector.feature_undo = Mock()
        def completed_surface(surface):
            if surface == "chromium":
                collector.rpc.side_effect = restoration
        collector.feature_status = Mock(side_effect=completed_surface)
        with self.assertRaises(Failure) as raised:
            collector.features()
        self.assertIs(raised.exception, restoration)
        self.assertEqual(collector.rpc.call_args.args, ("chromium", "status"))
        self.assertEqual(collector.rpc.call_args.kwargs, {"enabled": True})

    def test_status_restore_eof_with_ambient_exception_remains_failure(self):
        collector = Collector({}, Mock(), None, None)
        restoration = Failure("observer EOF")
        collector.rpc = Mock(return_value={"enabled": True})
        collector.feature_edit = Mock()
        collector.feature_tab = Mock()
        collector.feature_undo = Mock()
        def completed_surface(surface):
            if surface == "chromium":
                collector.rpc.side_effect = restoration
        collector.feature_status = Mock(side_effect=completed_surface)
        try:
            raise Failure("unrelated caller failure")
        except Failure:
            with self.assertRaises(Failure) as raised:
                collector.features()
        self.assertIs(raised.exception, restoration)

    def test_feature_prepare_passes_target_without_changing_default(self):
        guest = Guest.__new__(Guest)
        guest.prepare = Mock(return_value={})
        guest.call({"surface": "gtk", "action": "prepare", "request_id": "one", "text": "", "target": "text_view"})
        guest.prepare.assert_called_once_with("", "text_view")

    def test_feature_selection_uses_physical_range_and_stops_before_wrong_transform(self):
        collector = Collector({}, Mock(), None, None)
        before = self.state("", 0)
        collector.feature_start = Mock(return_value=("entry", before, 0))
        collector.type_keys, collector.key = Mock(), Mock()
        collector.wait_state = Mock(side_effect=[before, before, Failure("wrong transform", "FAIL_PRODUCT")])
        collector.feature_finish = Mock()
        with self.assertRaises(Failure):
            collector.feature_edit("gtk", "case-selection")
        collector.type_keys.assert_called_once_with("gtk", "hELLo zz")
        self.assertEqual([call.args[1:] for call in collector.key.call_args_list],
                         [("home",)] + [("right",)] * 5 + [("shift", "home"), ("alt", "pause")])
        self.assertEqual(collector.wait_state.call_args_list[1].args[3:], ("hELLo zz", [0, 5], 0))
        collector.feature_finish.assert_not_called()

    def test_tab_wrong_destination_cannot_adopt_owner_or_send_following_key(self):
        from copy import deepcopy
        before, restored = self.tab_states()
        before["state"]["following_text"] = "untouched-sentinel"
        restored["state"]["following_text"] = "untouched-sentinel"
        collector = Collector({}, Mock(), None, None)
        collector.baseline = deepcopy(before)
        collector.feature_start = Mock(return_value=("entry", before, 0))
        collector.type_keys, collector.key, collector.feature_finish = Mock(), Mock(), Mock()
        collector.wait_state = Mock(side_effect=[before, restored])
        destination = deepcopy(restored)
        destination["state"].update(text="corrupted", selection=[0, 0], following_text="привет")
        collector.rpc = Mock(return_value=destination)
        with self.assertRaises(Failure):
            collector.feature_tab("gtk")
        self.assertEqual(collector.baseline["clipboard"], before["clipboard"])
        self.assertEqual([call.args[1:] for call in collector.key.call_args_list], [("tab",)])
        collector.feature_finish.assert_not_called()

    def test_immediate_undo_uses_trigger_not_snapshot_time(self):
        collector = Collector({}, Mock(), None, None)
        before = self.state("", 0)
        before["state"]["keys"] = {"z_up": 0, "z_down": 0}
        collector.feature_start = Mock(return_value=("entry", before, 0))
        collector.type_keys = Mock()
        collector.key = Mock(side_effect=[100, Failure("too late")])
        collector.wait_state = Mock(return_value=before)
        collector.feature_finish = Mock()
        with self.assertRaises(Failure):
            collector.feature_undo("gtk", "immediate")
        self.assertEqual(collector.key.call_args.kwargs, {"deadline_ns": 2_500_000_100})
        collector.feature_finish.assert_not_called()


    def expiry_probe(self, samples=None, *, late=False, identity_drift=False):
        from copy import deepcopy
        clock = [10_000_000_000]
        before = self.state("", 0)
        before["state"].update(selection=[0, 0], keys={"z_down": 0, "z_up": 0})
        collector = Collector({}, Mock(), None, None)
        collector.baseline = deepcopy(before)
        collector.feature_start = Mock(return_value=("entry", before, 0))
        collector.type_keys, collector.feature_finish = Mock(), Mock()
        z_times, triggers, completions = [], [], []
        values = [{"daemon_epoch": 7, "word_dispatches": 4, "input_in_flight": 0,
                   "pending_words": 0, "ready_results": 0, "analysis_outstanding": 0,
                   "queued_tasks": 0}] + (samples or [{"daemon_epoch": 7, "word_dispatches": 5}])
        last = values[0]
        def key(surface, *keys, **kwargs):
            if keys == ("ctrl", "pause"):
                triggers.append(clock[0])
            if keys == ("ctrl", "z"):
                z_times.append(clock[0])
            return clock[0]
        collector.key = Mock(side_effect=key)
        def state_wait(surface, widget, original, text, selection, group):
            if text == "HellO":
                clock[0] += 700_000_000
            result = deepcopy(before)
            result["state"].update(text=text, selection=selection)
            return result
        collector.wait_state = Mock(side_effect=state_wait)
        def rpc(surface, action, **kwargs):
            nonlocal last
            result = deepcopy(before)
            if action == "stats":
                if values:
                    last = {**last, **values.pop(0)}
                clock[0] += 100_000_000
                if late and triggers:
                    clock[0] = triggers[0] + 5_000_000_001
                result["stats"] = last.copy()
                if identity_drift and triggers:
                    result["identity"]["daemon"] = [43, "123"]
                if last["word_dispatches"] == 5:
                    completions.append(clock[0])
            else:
                result["state"].update(text="HellO", selection=[5, 5],
                                       keys={"z_down": 1, "z_up": 1})
            return result
        collector.rpc = Mock(side_effect=rpc)
        def sleep(seconds):
            clock[0] += max(1, int(seconds * 1e9))
        return collector, clock, z_times, triggers, completions, sleep

    def test_expiry_undo_waits_from_completed_dispatch_not_trigger_or_dom(self):
        for surface in ("gtk", "chromium"):
            with self.subTest(surface=surface):
                collector, clock, z_times, triggers, completed, sleep = self.expiry_probe()
                with patch("installed_guest.time.monotonic_ns", side_effect=lambda: clock[0]), \
                        patch("installed_guest.time.monotonic", side_effect=lambda: clock[0] / 1e9), \
                        patch("installed_guest.time.sleep", side_effect=sleep):
                    collector.feature_undo(surface, "expiry")
                self.assertGreaterEqual(z_times[0], triggers[0] + 700_000_000 + 2_600_000_000)
                self.assertTrue(completed)
                self.assertGreaterEqual(z_times[0], completed[-1] + 2_600_000_000)
                self.assertEqual(len(z_times), 1)
                collector.feature_finish.assert_called_once()

    def test_expiry_undo_requires_one_coherent_dispatch_and_drain_sample(self):
        for busy in ("input_in_flight", "pending_words", "ready_results", "analysis_outstanding", "queued_tasks"):
            with self.subTest(busy=busy):
                samples = [{"word_dispatches": 4, busy: 1},
                           {"word_dispatches": 5, busy: 1},
                           {"word_dispatches": 5, busy: 0}]
                collector, clock, z_times, _triggers, completed, sleep = self.expiry_probe(samples)
                with patch("installed_guest.time.monotonic_ns", side_effect=lambda: clock[0]), \
                        patch("installed_guest.time.monotonic", side_effect=lambda: clock[0] / 1e9), \
                        patch("installed_guest.time.sleep", side_effect=sleep):
                    collector.feature_undo("gtk", "expiry")
                self.assertEqual(sum(call.args[1] == "stats" for call in collector.rpc.call_args_list), 4)
                self.assertGreaterEqual(z_times[0], completed[-1] + 2_600_000_000)
                deadlines = [call.kwargs["deadline_ns"] for call in collector.rpc.call_args_list
                             if call.args[1] == "stats"][1:]
                self.assertEqual(len(set(deadlines)), 1)

    def test_expiry_undo_refuses_stale_extra_epoch_identity_and_late_completion(self):
        cases = ({"samples": [{"word_dispatches": 4}]},
                 {"samples": [{"word_dispatches": 3}]},
                 {"samples": [{"word_dispatches": 6}]},
                 {"samples": [{"word_dispatches": 5, "pending_words": 1}, {"word_dispatches": 4}]},
                 {"samples": [{"daemon_epoch": 8, "word_dispatches": 5}]},
                 {"late": True}, {"identity_drift": True})
        for options in cases:
            with self.subTest(options=options):
                collector, clock, z_times, _triggers, _completed, sleep = self.expiry_probe(**options)
                with patch("installed_guest.time.monotonic_ns", side_effect=lambda: clock[0]), \
                        patch("installed_guest.time.monotonic", side_effect=lambda: clock[0] / 1e9), \
                        patch("installed_guest.time.sleep", side_effect=sleep), self.assertRaises(Failure):
                    collector.feature_undo("gtk", "expiry")
                self.assertEqual(z_times, [])
                collector.feature_finish.assert_not_called()

    def test_feature_stats_uses_authenticated_reader_without_learning_phase(self):
        guest = Guest.__new__(Guest)
        guest.m = {"features": True}
        authority = self.state("", 0)
        guest.system = Mock(return_value=authority)
        guest.target, guest.require_owned_learning_phase = Mock(return_value={}), Mock()
        with patch("installed_guest.public_stats", return_value={"daemon_epoch": 7}) as stats:
            observed = guest.call({"surface": "gtk", "action": "stats"})
        self.assertIn("stats", observed)
        self.assertEqual(observed["stats"], {"daemon_epoch": 7})
        stats.assert_called_once_with(42)
        guest.require_owned_learning_phase.assert_not_called()
        guest.target.assert_not_called()

    def test_feature_stats_rejects_peer_context_or_held_key_drift(self):
        for field in ("identity", "clipboard", "pressed"):
            guest = Guest.__new__(Guest)
            guest.m = {"features": True}
            before, after = self.state("", 0), self.state("", 0)
            after[field] = {"daemon": [43, "123"]} if field == "identity" else ["ctrl"] if field == "pressed" else "foreign"
            guest.system = Mock(side_effect=[before, after, after])
            guest.target = Mock(return_value={})
            with self.subTest(field=field), patch("installed_guest.public_stats", return_value={}), self.assertRaises(Failure):
                guest.call({"surface": "gtk", "action": "stats"})

    def test_runtime_status_whitelist_ack_and_independent_readback(self):
        from installed_guest import runtime_status
        def client(reply):
            value = Mock()
            value.recv.return_value = reply
            context = Mock()
            context.__enter__ = Mock(return_value=value)
            context.__exit__ = Mock(return_value=False)
            return context
        with patch("installed_guest.socket.socket", side_effect=[client(b"OK DISABLED\n"), client(b"OK DISABLED\n")]) as sockets:
            self.assertFalse(runtime_status(False))
            self.assertEqual(sockets.call_count, 2)
        for response in (b"OK Scheduled\n", b"ERROR denied\n", b"OK ENABLED\n"):
            with patch("installed_guest.socket.socket", return_value=client(response)), self.assertRaises(Failure):
                runtime_status(False)
        with patch("installed_guest.socket.socket") as sockets, self.assertRaises(Failure):
            runtime_status("0")
        sockets.assert_not_called()

    def test_status_cleanup_requires_original_readback(self):
        collector = Collector({}, Mock(), None, None)
        collector.rpc = Mock(side_effect=[{"enabled": False}, {}, {"enabled": True}])
        collector.feature_edit = Mock(side_effect=Failure("case failed"))
        with self.assertRaisesRegex(Failure, "original runtime status not restored"):
            collector.features()

    def test_public_status_wrong_daemon_peer_never_sends_command(self):
        import struct
        from installed_guest import runtime_status
        client = Mock()
        context = Mock(__enter__=Mock(return_value=client), __exit__=Mock(return_value=False))
        for pid, uid in ((999, 0), (42, 1000)):
            client.getsockopt.return_value = struct.pack("3i", pid, uid, 0)
            with patch("installed_guest.socket.socket", return_value=context), self.assertRaises(Failure):
                runtime_status(False, expected_pid=42)
        client.sendall.assert_not_called()

    def test_plain_clipboard_keeps_compound_text_bytes_type_and_format(self):
        import ast
        import base64
        tree = ast.parse((Path(__file__).parent / "gtk_target.py").read_text())
        method = next(node for node in ast.walk(tree) if isinstance(node, ast.FunctionDef) and node.name == "clipboard")
        clipboard = Mock()
        clipboard.wait_for_targets.return_value = (True, [Mock(**{"name.return_value": name})
                                                          for name in ("TARGETS", "COMPOUND_TEXT", "UTF8_STRING")])
        data = Mock()
        data.get_length.return_value = 4
        data.get_data.return_value = b"\x1b-Ax"
        data.get_data_type.return_value.name.return_value = "COMPOUND_TEXT"
        data.get_format.return_value = 8
        clipboard.wait_for_contents.return_value = data
        gtk, gdk = Mock(), Mock()
        gtk.Clipboard.get.return_value = clipboard
        namespace = {"Gtk": gtk, "Gdk": gdk, "base64": base64}
        exec(compile(ast.Module(body=[method], type_ignores=[]), "gtk_target.py:clipboard", "exec"), namespace)
        actual = namespace["clipboard"](None)
        self.assertEqual(actual["COMPOUND_TEXT"], ["COMPOUND_TEXT", 8, "Gy1BeA=="])

    def test_repeat_same_field_stops_on_wrong_inverse_before_third_pause(self):
        def state(text, group):
            value = self.state(text, group)
            value["state"]["selection"] = [len(text)] * 2
            return value
        for bad_inverse in (False, True):
            with self.subTest(bad_inverse=bad_inverse), tempfile.TemporaryDirectory() as directory:
                collector = self.clearing_collector(directory)
                current = state("", 0)
                snapshots = iter([current, current, current, state("ghbdtn", 0), state("привет", 1),
                                  state("wrong", 1) if bad_inverse else state("ghbdtn", 0),
                                  state("привет", 1), state("приветф", 1)])
                def rpc(surface, action, **value):
                    nonlocal current
                    if action == "identity":
                        return {**current, "observer_id": surface, "observer_instance_id": "actual-instance", "observer_ns": 100,
                                "host_interval_ns": [200, 240]}
                    if action == "snapshot":
                        current = next(snapshots)
                    return current
                collector.rpc = Mock(side_effect=rpc)
                with patch("installed_guest.time.monotonic", side_effect=lambda: 6 if current["state"]["text"] == "wrong" else 0):
                    if bad_inverse:
                        with self.assertRaises(Failure):
                            collector.operation("gtk", 0, "repeat")
                    else:
                        collector.operation("gtk", 0, "repeat")
                keys = [call.args[1:] for call in collector.key.call_args_list]
                prefix = [("ctrl", "a"), ("backspace",)] + [(key,) for key in "ghbdtn"]
                self.assertEqual(keys, prefix + [("pause",)] * (2 if bad_inverse else 3) + ([] if bad_inverse else [("a",)]))
                verbs = [call.args[1] for call in collector.rpc.call_args_list]
                self.assertEqual(verbs.count("prepare"), 1)
                self.assertEqual(verbs.count("arm"), 2)
                self.assertEqual(verbs.count("complete"), 1 if bad_inverse else 2)
                self.assertEqual(collector.completed, 0 if bad_inverse else 1)
                self.assertEqual((Path(directory) / "operations.jsonl").exists(), not bad_inverse)

    def clearing_collector(self, directory, group=0, bad=None):
        baseline = self.state("", group)
        baseline["state"]["selection"] = [0, 0]
        collector = Collector({"run_id": "run"}, Mock(), None, Path(directory))
        snapshots = 0
        def rpc(surface, action, **value):
            nonlocal snapshots
            if action == "arm" and not value["case_id"].endswith("-physical-clear"):
                raise Failure("stop at product binding")
            if action == "identity":
                return {**baseline, "observer_id": surface, "observer_instance_id": "actual-instance", "observer_ns": 100,
                        "host_interval_ns": [200, 240]}
            if action == "snapshot":
                snapshots += 1
                return bad if bad and snapshots > 1 else baseline
            return baseline
        collector.rpc, collector.key = Mock(side_effect=rpc), Mock()
        return collector

    def test_physical_clear_precedes_product_binding_without_count(self):
        with tempfile.TemporaryDirectory() as directory:
            collector = self.clearing_collector(directory)
            with self.assertRaises(Failure):
                collector.operation("gtk", 0)
            self.assertEqual([call.args[1:] for call in collector.key.call_args_list], [("ctrl", "a"), ("backspace",)])
            verbs = [call.args[1] for call in collector.rpc.call_args_list]
            self.assertLess(verbs.index("complete"), len(verbs) - 1)
            self.assertEqual(collector.completed, 0)
            self.assertFalse((Path(directory) / "operations.jsonl").exists())

    def test_initial_ru_group_is_admitted_without_repair(self):
        with tempfile.TemporaryDirectory() as directory:
            collector = self.clearing_collector(directory, group=1)
            with self.assertRaises(Failure):
                collector.operation("gtk", None)
            arms = [call.kwargs["case_id"] for call in collector.rpc.call_args_list if call.args[1] == "arm"]
            self.assertTrue(arms[0].startswith("gtk-1-"))
            self.assertEqual(collector.key.call_count, 2)

    def test_unexpected_or_unknown_initial_group_never_arms_or_types(self):
        for actual, tracked in ((0, 1), (2, None)):
            with self.subTest(actual=actual, tracked=tracked), tempfile.TemporaryDirectory() as directory:
                collector = self.clearing_collector(directory, group=actual)
                collector.last_group = tracked
                with self.assertRaises(Failure):
                    collector.operation("gtk", None)
                collector.key.assert_not_called()
                self.assertFalse(any(call.args[1] == "arm" for call in collector.rpc.call_args_list))

    def test_failed_clear_cannot_arm_product_or_count(self):
        mutations = [("state", {"text": "residual", "selection": [0, 0], "following_text": "other"}),
                     ("state", {"text": "", "selection": [1, 1], "following_text": "other"}),
                     ("state", {"text": "", "selection": [0, 0], "following_text": "changed"}),
                     ("clipboard", "lost"), ("identity", "changed"), ("foreground", False), ("pressed", ["Control_L"])]
        for field, value in mutations:
            with self.subTest(field=field, value=value), tempfile.TemporaryDirectory() as directory:
                bad = self.state("", 0)
                bad["state"]["selection"] = [0, 0]
                bad[field] = value
                collector = self.clearing_collector(directory, bad=bad)
                with patch("installed_guest.time.monotonic", side_effect=range(0, 100, 6)), self.assertRaises(Failure):
                    collector.operation("gtk", 0)
                self.assertEqual([call.args[1:] for call in collector.key.call_args_list], [("ctrl", "a"), ("backspace",)])
                self.assertGreaterEqual(sum(call.args[1] == "snapshot" for call in collector.rpc.call_args_list), 2)
                self.assertTrue(all(call.kwargs["case_id"].endswith("-physical-clear") for call in collector.rpc.call_args_list if call.args[1] == "arm"))
                self.assertEqual(collector.completed, 0)
                self.assertFalse((Path(directory) / "operations.jsonl").exists())

    def test_ctrl_a_and_backspace_whitelist_preserve_order(self):
        events = json.loads(qmp_keys(["ctrl", "a"]))["arguments"]["events"]
        self.assertEqual([(e["data"]["key"]["data"], e["data"]["down"]) for e in events],
                         [("ctrl", True), ("a", True), ("a", False), ("ctrl", False)])
        self.assertEqual(len(json.loads(qmp_keys(["backspace"]))["arguments"]["events"]), 2)
        for chord in (["ctrl", "p"], ["ctrl"]):
            with self.assertRaises(Failure):
                qmp_keys(chord)

    def test_reset_snapshot_must_have_fresh_serial(self):
        with tempfile.TemporaryDirectory() as directory:
            channel = Mock()
            channel.call.return_value = {"observer_instance_id": "instance", "observation_id": 7}
            collector = Collector({}, None, channel, Path(directory))
            collector.rpc("gtk", "snapshot", target="entry")
            with self.assertRaises(Failure):
                collector.rpc("gtk", "snapshot", target="entry")
            self.assertEqual(collector.completed, 0)

    def test_gtk_transport_preserves_real_observer_cycle_without_outer_id(self):
        code = """import sys,json
sys.path.insert(0, sys.argv[1])
from probe import ObservationProtocol
p=ObservationProtocol('gtk')
for line in sys.stdin:
    r=json.loads(line); a=r['action']; v={}
    if a=='identity': v={'observer_id':p.observer_id,'observer_instance_id':p.observer_instance_id}
    elif a=='arm': p.arm(r['run_id'],r['case_id'],r['attempt_id'],r['clock_mapping'])
    elif a=='snapshot': v=p.snapshot({'text':'','selection':[0,0]})
    elif a=='complete': p.complete()
    print(json.dumps({'ok':True,'received':r,'active':p.active,**v}),flush=True)
"""
        guest = Guest.__new__(Guest)
        guest.gtk = Lines([sys.executable, "-u", "-c", code, str(Path(__file__).parent)])
        guest.system = Mock(return_value=self.state("", 0))
        def call(action, **fields):
            result = guest.call({"surface": "gtk", "action": action, "request_id": "outer-id", **fields})
            self.assertEqual(result["received"], {"action": action, **fields})
            return result
        try:
            identity = call("identity")
            mapping = {"observer_id": "gtk", "observer_instance_id": identity["observer_instance_id"],
                       "observer_domain": "collector_monotonic_ns", "collector_domain": "collector_monotonic_ns",
                       "offset_ns": 1, "mapping_id": "mapped", "calibration": {"source": "collector_handshake", "calibration_id": "measured"}}
            self.assertTrue(call("arm", run_id="run", case_id="case", attempt_id="attempt", clock_mapping=mapping)["active"])
            snapshot = call("snapshot", target="entry")
            self.assertEqual([snapshot[k] for k in ("run_id", "case_id", "attempt_id")], ["run", "case", "attempt"])
            self.assertEqual(snapshot["clock"]["offset_ns"], 1)
            self.assertFalse(call("complete")["active"])
        finally:
            guest.gtk.close()

    def test_browser_transport_keeps_semantic_fields_without_outer_id(self):
        guest = Guest.__new__(Guest)
        guest.m = {"run_id": "run"}
        guest.page = Mock(url=(Path(__file__).parent / "chromium_target.html").as_uri() + "?run=run")
        guest.page.evaluate.side_effect = lambda expression, value: {"received": value}
        guest.system = Mock(return_value=self.state("", 0))
        actions = [("identity", {}), ("arm", {"run_id": "run", "case_id": "case", "attempt_id": "attempt", "clock_mapping": {"offset_ns": 1}}),
                   ("snapshot", {"target": "input"}), ("complete", {})]
        for action, fields in actions:
            with self.subTest(action=action):
                result = guest.call({"surface": "chromium", "action": action, "request_id": "outer-id", **fields})
                self.assertEqual(result["received"], fields)

    def active_guest(self, actual=0, size=0, count=0, after=0, status=0, active=99):
        guest = Guest.__new__(Guest)
        guest.display, guest.x = None, Mock()
        allocation = C.c_ulong(active)
        def read(*args):
            for position, value in ((7, actual), (8, size), (9, count), (10, after)):
                args[position]._obj.value = value
            if actual:
                C.cast(args[11], C.POINTER(C.POINTER(C.c_ubyte)))[0] = C.cast(C.pointer(allocation), C.POINTER(C.c_ubyte))
            return status
        guest.x.XGetWindowProperty.side_effect = read
        return guest

    def test_native_missing_active_property_is_pending(self):
        guest = self.active_guest()
        self.assertIsNone(guest.active_window())
        guest.x.XFree.assert_not_called()

    def test_native_active_property_uses_window_and_frees_memory(self):
        guest = self.active_guest(actual=33, size=32, count=1)
        self.assertEqual(guest.active_window(), 99)
        guest.x.XFree.assert_called_once()

    def test_native_malformed_property_is_not_pending(self):
        for actual, size, count, after in ((6, 32, 1, 0), (33, 16, 1, 0), (33, 32, 2, 0), (33, 32, 1, 1)):
            guest = self.active_guest(actual, size, count, after)
            with self.assertRaises(Failure):
                guest.active_window()
            guest.x.XFree.assert_called_once()

    def test_native_property_read_error_is_not_pending(self):
        guest = self.active_guest(status=1)
        with self.assertRaises(Failure):
            guest.active_window()

    def preparation_guest(self, states):
        guest = Guest.__new__(Guest)
        guest.surface = "gtk"
        guest.target = Mock(return_value={"ok": True})
        guest.system = Mock(side_effect=states)
        return guest

    def test_prepare_waits_for_owned_foreground(self):
        pending, ready = self.state("", 0), self.state("", 0)
        pending["foreground"] = False
        guest = self.preparation_guest([pending, pending, ready])
        with patch("installed_guest.time.sleep"):
            result = guest.call({"surface": "gtk", "action": "prepare"})
        self.assertTrue(result["foreground"])
        self.assertEqual(guest.target.call_count, 2)
        self.assertEqual(guest.system.call_count, 3)

    def test_prepare_focus_timeout_is_finite(self):
        pending = self.state("", 0)
        pending["foreground"] = False
        guest = self.preparation_guest([pending, pending])
        with patch("installed_guest.time.monotonic", side_effect=[0, 1, 6]), patch("installed_guest.time.sleep"), self.assertRaises(Failure):
            guest.call({"surface": "gtk", "action": "prepare"})
        self.assertEqual(guest.target.call_count, 2)
        self.assertLessEqual(guest.system.call_count, 2)

    def test_prepare_authority_drift_stops_immediately(self):
        for field in ("identity", "clipboard"):
            for foreground in (False, True):
                before, changed = self.state("", 0), self.state("", 0)
                before["foreground"], changed["foreground"] = False, foreground
                changed[field] = "drift"
                guest = self.preparation_guest([before, changed])
                with self.assertRaises(Failure) as error:
                    guest.call({"surface": "gtk", "action": "prepare"})
                self.assertEqual(error.exception.kind, "ABORTED_SAFETY")
                self.assertEqual(guest.system.call_count, 2)

    def test_prepare_modifier_during_wait_is_not_ready(self):
        before, held = self.state("", 0), self.state("", 0)
        before["foreground"] = False
        held["pressed"] = ["Shift_L"]
        guest = self.preparation_guest([before, held])
        with self.assertRaises(Failure) as error:
            guest.call({"surface": "gtk", "action": "prepare"})
        self.assertEqual(error.exception.kind, "ABORTED_SAFETY")
        self.assertEqual(guest.system.call_count, 2)

    def test_prepare_held_modifier_never_presents(self):
        held = self.state("", 0)
        held["pressed"] = ["Shift_L"]
        guest = self.preparation_guest([held])
        with self.assertRaises(Failure):
            guest.call({"surface": "gtk", "action": "prepare"})
        guest.target.assert_not_called()

    def test_prepare_provider_failure_is_not_retried(self):
        guest = self.preparation_guest([self.state("", 0), Failure("process failed")])
        with self.assertRaises(Failure):
            guest.call({"surface": "gtk", "action": "prepare"})
        self.assertEqual(guest.system.call_count, 2)

    def test_bad_prepare_receipt_never_arms(self):
        collector = Collector({}, Mock(), None, None)
        pending = self.state("", 0)
        pending["foreground"] = False
        collector.rpc = Mock(side_effect=[pending, Failure("unexpected post-prepare step")])
        with self.assertRaises(Failure):
            collector.operation("gtk", 0)
        self.assertEqual(collector.rpc.call_count, 1)
        collector.guard.send.assert_not_called()

    def test_existing_private_guest_group_needs_no_setsid(self):
        with patch("installed_guest.os.getpid", return_value=42), patch("installed_guest.os.getpgrp", return_value=42), patch("installed_guest.os.setsid") as setsid:
            self.assertEqual(guest_group(), 42)
            setsid.assert_not_called()

    def test_shared_guest_group_becomes_private(self):
        with patch("installed_guest.os.getpid", return_value=42), patch("installed_guest.os.getpgrp", side_effect=[7, 42]), patch("installed_guest.os.setsid") as setsid:
            self.assertEqual(guest_group(), 42)
            setsid.assert_called_once_with()

    def test_shared_group_failure_never_kills(self):
        with patch("installed_guest.os.getpid", return_value=42), patch("installed_guest.os.getpgrp", return_value=7), patch("installed_guest.os.setsid", side_effect=PermissionError), patch("installed_guest.os.killpg") as kill:
            with self.assertRaises(PermissionError):
                guest_group()
            with self.assertRaises(Failure):
                kill_guest_group(42)
            kill.assert_not_called()

    def test_stderr_is_retained_but_capped(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "guest-stderr.log"
            lines = Lines([sys.executable, "-c", "import sys; sys.stderr.write('x'*100000); print('{}',flush=True)"], stderr_path=path)
            self.assertEqual(lines.read(), {})
            lines.close()
            self.assertEqual(path.read_bytes(), b"x" * 65536)

    def test_wrong_uuid_never_injects(self):
        run = Mock(return_value="other-uuid")
        with self.assertRaises(Failure):
            Guard("punto-r23-native", "expected-uuid", run).send(["pause"])
        self.assertFalse(any("qemu-monitor-command" in call.args[0] for call in run.call_args_list))

    def test_wrong_domain_never_runs_command(self):
        run = Mock()
        with self.assertRaises(Failure):
            Guard("workstation", "uuid", run).send(["pause"])
        run.assert_not_called()

    def test_stopped_guest_never_injects(self):
        run = Mock(side_effect=["uuid", "shut off"])
        with self.assertRaises(Failure):
            Guard("punto-r23-native", "uuid", run).send(["pause"])
        self.assertEqual(run.call_count, 2)

    def test_qmp_error_is_not_retried(self):
        run = Mock(side_effect=["uuid", "running", '{"error":{"desc":"failed"}}'])
        with self.assertRaises(Failure):
            Guard("punto-r23-native", "uuid", run).send(["pause"])
        self.assertEqual(run.call_count, 3)

    def test_semantic_space_uses_qmp_spc_without_expanding_whitelist(self):
        events = json.loads(qmp_keys(["space"]))["arguments"]["events"]
        self.assertEqual([(e["data"]["key"]["data"], e["data"]["down"]) for e in events],
                         [("spc", True), ("spc", False)])
        with self.assertRaises(Failure):
            qmp_keys(["spc"])

    def test_ordered_chord_releases_in_reverse(self):
        events = json.loads(qmp_keys(["shift", "pause"]))["arguments"]["events"]
        self.assertEqual([(e["data"]["key"]["data"], e["data"]["down"]) for e in events],
                         [("shift", True), ("pause", True), ("pause", False), ("shift", False)])

    def test_non_whitelisted_key_rejected(self):
        with self.assertRaises(Failure):
            qmp_keys(["ctrl", "alt", "delete"])

    def test_command_timeout_remains_precondition_failure(self):
        with self.assertRaises(Failure) as error:
            checked_run(["/usr/bin/sleep", "1"], timeout=0.01)
        self.assertEqual(error.exception.kind, "BLOCKED_PRECONDITION")

    def test_source_drift(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "source"
            path.write_text("changed")
            with self.assertRaises(Failure):
                check_files({str(path): "0" * 64})

    def test_ordinary_targets_reject_module_and_forced_environment(self):
        for name in ("GTK_MODULES", "NO_AT_BRIDGE", "GTK_IM_MODULE_FILE"):
            with self.assertRaises(Failure):
                ordinary_environment({"XDG_SESSION_TYPE": "x11", name: "forced"})

    def test_stale_snapshot_is_not_fresh_evidence(self):
        before, after = self.state("ghbdtn", 0), self.state("привет", 1)
        before["observation_id"] = after["observation_id"] = 1
        with self.assertRaises(Failure):
            verify_state(before, after, "привет", [6, 6], 1)

    def test_idle_checks_cannot_ignore_clipboard_loss(self):
        collector = Collector({}, None, None, None)
        collector.baseline = self.state("ghbdtn", 0)
        bad = self.state("ghbdtn", 0)
        bad["clipboard"] = "lost"
        collector.rpc = Mock(return_value=bad)
        with self.assertRaises(Failure):
            collector.check("gtk")

    def test_lost_foreground_prevents_physical_send(self):
        guard = Mock()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "package"
            path.write_bytes(b"")
            manifest = {"sources": {}, "package": {"path": str(path), "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}}
            collector = Collector(manifest, guard, None, Path(directory))
            state = self.state("", 0)
            state["foreground"] = False
            collector.rpc = Mock(return_value=state)
            with self.assertRaises(Failure):
                collector.key("gtk", "pause")
            guard.send.assert_not_called()

    def test_response_correlation_rejects_stale_reply(self):
        lines = Lines.__new__(Lines)
        lines.child = Mock()
        lines.send = Mock()
        lines.read = Mock(return_value={"request_id": "stale"})
        with self.assertRaises(Failure):
            lines.call({"request_id": "current"})

    def test_pipe_write_has_a_deadline(self):
        lines = Lines.__new__(Lines)
        lines.child = Mock()
        with patch("installed_guest.select.select", return_value=([], [], [])), self.assertRaises(Failure):
            lines.send({"action": "check"}, timeout=0.001)

    def test_expired_request_is_refused_before_any_pipe_dispatch(self):
        import os
        read_fd, write_fd = os.pipe()
        try:
            os.set_blocking(read_fd, False)
            with os.fdopen(write_fd, "wb", buffering=0) as writer:
                write_fd = None
                lines = Lines.__new__(Lines)
                lines.child = Mock(stdin=writer)
                with patch("installed_guest.select.select") as ready, \
                        patch("installed_guest.os.write") as write, \
                        self.assertRaises(Failure) as rejected:
                    lines.send({"action": "snapshot"}, deadline_ns=1)
                self.assertEqual(rejected.exception.kind, "INVALID_OBSERVATION")
                self.assertIsNone(getattr(lines, "terminal_failure", None))
                self.assertIn("before dispatch", str(rejected.exception))
                ready.assert_not_called()
                write.assert_not_called()
                with self.assertRaises(BlockingIOError):
                    os.read(read_fd, 1)
            self.assertEqual(os.read(read_fd, 1), b"")
        finally:
            os.close(read_fd)
            if write_fd is not None:
                os.close(write_fd)

    def test_terminal_receipt_drift_prevents_acceptance(self):
        collector = Collector({}, None, None, None)
        collector.baseline = self.state("", 0)
        for field in ("clipboard", "identity"):
            terminal = self.state("", 0)
            terminal[field] = "changed-after-final-snapshot"
            with self.assertRaises(Failure):
                collector.authority(terminal, foreground=True)

    def test_chromium_nonce_cannot_be_replaced_by_foreign_tab(self):
        guest = Guest.__new__(Guest)
        guest.m = {"run_id": "expected"}
        guest.page = Mock(url="file:///other.html?run=expected")
        with self.assertRaises(Failure):
            guest.page_identity()

    def test_operation_failure_writes_no_success_receipt_or_count(self):
        identity = {"observer_id": "gtk", "observer_instance_id": "real-instance", "observer_ns": 100,
                    "host_interval_ns": [200, 240]}
        before, after = self.state("", 0), self.state("wrong", 0)
        before["state"]["selection"] = [0, 0]
        with tempfile.TemporaryDirectory() as directory:
            collector = Collector({"run_id": "run"}, None, None, Path(directory))
            collector.rpc = Mock(side_effect=[before, identity, {}, before, before, before, identity, {}, before, after])
            collector.key = Mock()
            with patch("installed_guest.time.monotonic", side_effect=[0, 0, 0, 6]), self.assertRaises(Failure):
                collector.operation("gtk", 0)
            self.assertEqual(collector.completed, 0)
            self.assertFalse((Path(directory) / "operations.jsonl").exists())
            arm = collector.rpc.call_args_list[2].kwargs["clock_mapping"]
            self.assertEqual(arm["offset_ns"], 120)
            self.assertEqual(arm["calibration"]["uncertainty_ns"], 20)
            self.assertEqual(arm["calibration"]["before"]["host_interval_ns"], [200, 240])
            arms = [call for call in collector.rpc.call_args_list if call.args[1] == "arm"]
            self.assertEqual([call.kwargs["case_id"] for call in arms], ["gtk-0-word-physical-clear", "gtk-0-word"])
            self.assertNotEqual(arms[0].kwargs["attempt_id"], arms[1].kwargs["attempt_id"])
            self.assertEqual(collector.key.call_count, 8)

    def test_idle_layout_drift_is_a_failure(self):
        collector = Collector({}, None, None, None)
        collector.baseline = self.state("", 0)
        collector.last_group = 0
        collector.rpc = Mock(return_value=self.state("", 1))
        with self.assertRaises(Failure):
            collector.check("gtk", idle=True)

    def test_failed_operation_not_accepted(self):
        before = self.state("ghbdtn", 0)
        after = self.state("ghbdtn", 0)
        with self.assertRaises(Failure) as error:
            verify_state(before, after, "привет", [6, 6], 1)
        self.assertEqual(error.exception.kind, "FAIL_PRODUCT")

    def test_selection_cannot_corrupt_suffix_or_clipboard(self):
        before = self.state("ghbdtn suffix", 0)
        for text, clipboard in (("привет", "sentinel"), ("привет suffix", "lost")):
            after = self.state(text, 1)
            after["clipboard"] = clipboard
            with self.assertRaises(Failure):
                verify_state(before, after, "привет suffix", [6, 6], 1)


    def lifecycle_authority(self):
        from copy import deepcopy
        before = self.state("", 0)
        before["identity"] = {r: [40 + i, str(100 + i)] for i, r in enumerate(
            ("udevmon", "interception", "daemon", "uinput", "helper", "gnome", "ibus", "tray"))}
        after = deepcopy(before)
        for r in ("udevmon", "interception", "daemon", "uinput"):
            after["identity"][r] = [after["identity"][r][0] + 100, "999"]
        return before, after

    def phase_capability_manifest(self):
        import base64, hashlib
        on = b"sound:\n  enabled: true"
        off = b"sound:\n  enabled: false"
        digest = "1" * 64
        return {"python": "/owned/python", "terminal_phase": True, "lifecycle_phase": True,
            "terminal": {"client": "/usr/bin/gnome-terminal", "client_sha256": digest,
                "server": "/usr/libexec/gnome-terminal-server", "server_sha256": digest,
                "python_sha256": digest},
            "processes": {"tray": "/usr/bin/punto-tray"}, "installed": {"/usr/bin/punto-tray": digest},
            "lifecycle": {"original_config_sha256": digest, "config_pair": {
                "on": base64.b64encode(on).decode(), "off": base64.b64encode(off).decode(),
                "on_sha256": hashlib.sha256(on).hexdigest(), "off_sha256": hashlib.sha256(off).hexdigest()},
                "tray": "/usr/bin/punto-tray", "tray_sha256": digest, "tray_bus_name": ":1.96",
                "tray_menu_path": "/org/ayatana/NotificationItem/punto_switcher/Menu", "audio": {
                    "tools": {name: digest for name in ("/usr/bin/pactl", "/usr/bin/parec", "/usr/bin/paplay")},
                    "waves": {"/usr/share/punto-switcher/sounds/en_ru.wav": digest,
                              "/usr/share/punto-switcher/sounds/ru_en.wav": digest}}}}

    def test_requested_phase_capabilities_reject_before_any_stage_or_guest_dispatch(self):
        import installed_guest
        from copy import deepcopy
        faults = [None, "no-terminal", "no-lifecycle", "terminal-relative", "terminal-hash",
                  "tray-pin", "audio-tool", "audio-wave", "pair-hash", "pair-schema"]
        faults += ["terminal-" + field for field in self.phase_capability_manifest()["terminal"]]
        faults += ["lifecycle-" + field for field in self.phase_capability_manifest()["lifecycle"]]
        for fault in faults:
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                manifest = deepcopy(self.phase_capability_manifest())
                manifest.update(domain_uuid="owned-domain", ssh_host="punto@192.0.2.8", sources={"owned": "1" * 64},
                    session={"XDG_SESSION_TYPE": "x11"}, package={"path": "/owned/package", "sha256": "1" * 64})
                manifest["processes"].update({role: "/owned/" + role for role in
                    (*PIPELINE_ROLES, "helper", "gnome", "ibus")})
                if fault in ("no-terminal", "no-lifecycle"):
                    del manifest[fault[3:]]
                elif fault == "terminal-relative":
                    manifest["terminal"]["client"] = "relative"
                elif fault == "terminal-hash":
                    manifest["terminal"]["python_sha256"] = "wrong"
                elif fault == "tray-pin":
                    manifest["lifecycle"]["tray_sha256"] = "2" * 64
                elif fault == "audio-tool":
                    del manifest["lifecycle"]["audio"]["tools"]["/usr/bin/parec"]
                elif fault == "audio-wave":
                    manifest["lifecycle"]["audio"]["waves"]["/usr/share/punto-switcher/sounds/ru_en.wav"] = "wrong"
                elif fault == "pair-hash":
                    manifest["lifecycle"]["config_pair"]["on_sha256"] = "2" * 64
                elif fault == "pair-schema":
                    manifest["lifecycle"]["config_pair"]["on"] = "not-base64"
                elif fault and fault.startswith("terminal-"):
                    del manifest["terminal"][fault[9:]]
                elif fault and fault.startswith("lifecycle-"):
                    del manifest["lifecycle"][fault[10:]]
                for name in ("key", "known_hosts"):
                    (root / name).write_text("owned scratch fixture")
                path = root / "manifest.json"
                path.write_text(json.dumps(manifest))
                args = ["installed_guest.py", "--vm", "owned", "--uuid", "owned-domain", "--host", manifest["ssh_host"],
                    "--key", str(root / "key"), "--known-hosts", str(root / "known_hosts"),
                    "--expected-payload-manifest", str(path), "--output", str(root / "output"), "--terminal", "--lifecycle"]
                caught = None
                with patch("installed_guest.sys.argv", args), patch("installed_guest.Guard") as guard, \
                        patch("installed_guest.checked_run") as command, patch("installed_guest.Lines") as lines:
                    guard.return_value.check.side_effect = Failure("passed capability boundary")
                    try:
                        installed_guest.main()
                    except Exception as error:
                        caught = error
                self.assertIsInstance(caught, Failure)
                if fault is None:
                    self.assertEqual(str(caught), "passed capability boundary")
                    guard.assert_called_once()
                else:
                    self.assertIn("capability", str(caught))
                    guard.assert_not_called()
                command.assert_not_called()
                lines.assert_not_called()
                self.assertFalse((root / "output").exists())

    def test_public_stats_only_initial_missing_socket_is_retryable_before_send(self):
        import os, socket, time
        import installed_guest
        original_socket = socket.socket
        for mode in ("missing", "refused", "expired", "late-missing"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                address = str(Path(directory) / "stats.sock")
                sent, connected = [], []
                clock = [0.0]
                class Client(original_socket):
                    def connect(self, _address):
                        connected.append(address)
                        if mode == "late-missing":
                            clock[0] = 2.0
                        return super().connect(address)
                    def sendall(self, data):
                        sent.append(data)
                        return super().sendall(data)
                listener = original_socket(socket.AF_UNIX, socket.SOCK_STREAM)
                try:
                    if mode == "refused":
                        listener.bind(address)  # Real existing but non-listening endpoint.
                    caught = None
                    with patch("installed_guest.socket.socket", Client), \
                            patch("installed_guest.time.monotonic", side_effect=lambda: clock[0]):
                        try:
                            if mode == "expired":
                                public_stats(os.getpid(), _deadline=-1)
                            else:
                                public_stats(os.getpid())
                        except Exception as error:
                            caught = error
                    if mode == "missing":
                        self.assertEqual(type(caught).__name__, "StatsNotPublished")
                        self.assertIsInstance(caught.__cause__, FileNotFoundError)
                    elif mode == "refused":
                        self.assertIsInstance(caught, ConnectionRefusedError)
                        self.assertNotEqual(type(caught).__name__, "StatsNotPublished")
                    else:
                        self.assertIsInstance(caught, Failure)
                        self.assertEqual(caught.kind, "INVALID_OBSERVATION")
                        self.assertEqual(connected, [] if mode == "expired" else [address])
                    self.assertEqual(sent, [])
                finally:
                    listener.close()

    def test_lifecycle_missing_ipc_polls_only_before_publication_and_same_identity(self):
        import socket
        import installed_guest
        from copy import deepcopy
        ready = {"x11_health": "ready", "analysis_health": "ready", "input_health": "ready",
            "learning_ready": 1, "learning_pending": 0, "learning_failed": 0,
            "learning_completed_generation": 2, "learning_generation": 2,
            "config_pending": 0, "config_result": "ok"}
        original_socket = socket.socket
        for mode in ("then-ready", "expiry", "identity-drift", "observer-drift", "after-publication", "ready-drift", "ready-late"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                before, after = self.lifecycle_authority()
                guest = Guest.__new__(Guest)
                phase = {"armed": True, "old_dead": True, "current": before, "transition_from": before}
                guest.learning_phase = phase
                changed = deepcopy(after)
                changed["identity"]["daemon"] = [999, "drift"]
                if mode == "observer-drift":
                    before["identity"]["gtk_observer"] = after["identity"]["gtk_observer"] = [777, "observer"]
                    changed = deepcopy(after)
                    changed["identity"]["gtk_observer"] = [777, "changed-start"]
                clock = [0.0]
                def observe(*_args, **_kwargs):
                    if guest.system_observation.call_count > 1:
                        if mode in ("identity-drift", "observer-drift", "ready-drift"):
                            return changed
                        if mode == "ready-late":
                            clock[0] = 4.0
                    return after
                guest.system_observation = Mock(side_effect=observe)
                guest.pipeline_admission = Mock(side_effect=lambda **_kwargs: ({},
                    changed["identity"] if guest.pipeline_admission.call_count > 1 and mode in
                    ("identity-drift", "ready-drift") else after["identity"]))
                guest.lifecycle_tray_identity = Mock()
                class Client(original_socket):
                    def connect(self, _address):
                        return super().connect(str(Path(directory) / "not-yet-published"))
                samples, deadlines = [], []
                def sample(pid, **kwargs):
                    samples.append(pid)
                    deadlines.append(kwargs.get("_deadline"))
                    if mode == "after-publication" and len(samples) == 1:
                        return {**ready, "config_pending": 1}
                    if mode in ("ready-drift", "ready-late"):
                        return ready
                    if mode == "then-ready" and len(samples) == 2:
                        return ready
                    with patch("installed_guest.socket.socket", Client):
                        return public_stats(pid, **kwargs)
                caught, result = None, None
                with patch("installed_guest.checked_run"), patch("installed_guest.public_stats", side_effect=sample), \
                        patch("installed_guest.time.monotonic", side_effect=lambda: clock[0]), \
                        patch("installed_guest.time.sleep", side_effect=lambda _value: clock.__setitem__(0, clock[0] + 1)):
                    try:
                        result = guest.lifecycle_start_pipeline()
                    except Exception as error:
                        caught = error
                if mode == "then-ready":
                    self.assertIsNone(caught, str(caught))
                    self.assertEqual(result["after"], after)
                    self.assertEqual(samples, [after["identity"]["daemon"][0]] * 2)
                    self.assertEqual(deadlines, [3.0, 3.0])
                else:
                    self.assertIsInstance(caught, Failure)
                    self.assertIs(phase["current"], before)
                    guest.lifecycle_tray_identity.assert_not_called()
                    if mode == "expiry":
                        self.assertEqual(clock[0], 3.0)
                        self.assertEqual(deadlines, [3.0] * 3)
                    elif mode in ("identity-drift", "observer-drift", "ready-drift", "ready-late"):
                        self.assertEqual(len(samples), 1)
                    else:
                        self.assertEqual(len(samples), 2)

    def test_lifecycle_readiness_preserves_first_terminal_failure_without_resend(self):
        before, after = self.lifecycle_authority()
        for kind in ("INVALID_OBSERVATION", "ABORTED_SAFETY", "BLOCKED_PRECONDITION"):
            with self.subTest(kind=kind):
                first = Failure("first bounded observer rejection", kind)
                first.add_note("original reader evidence")
                guest = Guest.__new__(Guest)
                guest.learning_phase = {"old_dead": True, "current": before, "transition_from": before}
                guest.pipeline_admission = Mock(return_value=({}, after["identity"]))
                guest.system_observation = Mock(side_effect=[first, BrokenPipeError("secondary dead reader")])
                guest.lifecycle_tray_identity = Mock()
                caught = None
                with patch("installed_guest.checked_run"), patch("installed_guest.time.sleep"), \
                        patch("installed_guest.public_stats") as stats:
                    try:
                        guest.lifecycle_start_pipeline()
                    except Exception as error:
                        caught = error
                self.assertIs(caught, first)
                self.assertEqual(caught.__notes__, ["original reader evidence"])
                guest.system_observation.assert_called_once_with({}, after["identity"], prepare_pending=True)
                stats.assert_not_called()
                guest.lifecycle_tray_identity.assert_not_called()
                self.assertIs(guest.learning_phase["current"], before)

        ready = {"x11_health": "ready", "analysis_health": "ready", "input_health": "ready",
                 "learning_ready": 1, "learning_pending": 0, "learning_failed": 0,
                 "learning_completed_generation": 2, "learning_generation": 2,
                 "config_pending": 0, "config_result": "ok"}
        for field, value in (("x11_health", "failed"), ("analysis_health", "failed"),
                             ("input_health", "failed"), ("learning_failed", 1), ("config_result", "error")):
            with self.subTest(terminal_field=field):
                guest = Guest.__new__(Guest)
                guest.learning_phase = {"old_dead": True, "current": before, "transition_from": before}
                guest.pipeline_admission = Mock(return_value=({}, after["identity"]))
                guest.system_observation = Mock(return_value=after)
                guest.lifecycle_tray_identity = Mock()
                caught = None
                clock = [0]
                with patch("installed_guest.checked_run"), \
                        patch("installed_guest.time.sleep", side_effect=lambda _delay: clock.__setitem__(0, 4)) as sleep, \
                        patch("installed_guest.time.monotonic", side_effect=lambda: clock[0]), \
                        patch("installed_guest.public_stats", side_effect=[{**ready, field: value}, ready]) as stats:
                    try:
                        guest.lifecycle_start_pipeline()
                    except Exception as error:
                        caught = error
                self.assertIsInstance(caught, Failure)
                self.assertEqual(caught.kind, "ABORTED_SAFETY")
                self.assertIn(field, str(caught))
                self.assertEqual(stats.call_count, 1)
                sleep.assert_not_called()
                guest.system_observation.assert_called_once_with({}, after["identity"], prepare_pending=True)
                guest.lifecycle_tray_identity.assert_not_called()
                self.assertIs(guest.learning_phase["current"], before)

    def test_lifecycle_readiness_only_validated_pending_stats_poll_original_deadline(self):
        import time
        before, after = self.lifecycle_authority()
        ready = {"x11_health": "ready", "analysis_health": "ready", "input_health": "ready",
                 "learning_ready": 1, "learning_pending": 0, "learning_failed": 0,
                 "learning_completed_generation": 2, "learning_generation": 2, "config_pending": 0, "config_result": "ok"}
        for mode in ("settle", "none", "degraded", "expiry", "terminal-stats"):
            with self.subTest(mode=mode):
                guest = Guest.__new__(Guest)
                guest.learning_phase = {"old_dead": True, "current": before, "transition_from": before}
                guest.pipeline_admission = Mock(return_value=({}, after["identity"]))
                guest.system_observation = Mock(return_value=after)
                guest.lifecycle_tray_identity = Mock()
                ready_sample = {**ready, "config_result": "none" if mode == "none" else "ok"}
                pending = {**ready_sample, "config_pending": 1}
                if mode == "degraded":
                    pending.update(x11_health="degraded", learning_ready=0, learning_pending=1)
                first = Failure("invalid STATS peer/schema", "INVALID_OBSERVATION")
                values = [pending, ready_sample] if mode in ("settle", "none", "degraded") else pending if mode == "expiry" else [first, BrokenPipeError()]
                clock = [0]
                with patch("installed_guest.checked_run"), \
                        patch("installed_guest.time.sleep", side_effect=lambda _delay: clock.__setitem__(0, clock[0] + 1.5)), \
                        patch("installed_guest.time.monotonic", **({"side_effect": lambda: clock[0]} if mode == "expiry" else {"wraps": time.monotonic})), \
                        patch("installed_guest.public_stats", **({"side_effect": values} if isinstance(values, list) else {"return_value": values})) as stats:
                    if mode in ("settle", "none", "degraded"):
                        result = guest.lifecycle_start_pipeline()
                        self.assertIs(result["after"], after)
                        self.assertEqual(stats.call_count, 2)
                    else:
                        caught = None
                        try:
                            guest.lifecycle_start_pipeline()
                        except Exception as error:
                            caught = error
                        self.assertIsNotNone(caught)
                        if mode == "terminal-stats":
                            self.assertIs(caught, first)
                            self.assertEqual(stats.call_count, 1)
                        else:
                            self.assertIsInstance(caught, Failure)
                            self.assertEqual(caught.kind, "ABORTED_SAFETY")
                            self.assertEqual(stats.call_count, 2)
                            self.assertEqual(clock[0], 3.0)

    def test_terminal_reader_failure_is_cached_before_future_pipe_write(self):
        import os
        import time
        read_fd, write_fd = os.pipe()
        try:
            os.set_blocking(read_fd, False)
            with os.fdopen(write_fd, "wb", buffering=0) as writer:
                write_fd = None
                lines = Lines.__new__(Lines)
                lines.child = Mock(stdin=writer)
                lines.buffer = bytearray(json.dumps({"ok": False, "terminal": True,
                    "kind": "INVALID_OBSERVATION", "error": "first snapshot rejection",
                    "notes": ["bounded original evidence"]}).encode() + b"\n")
                with self.assertRaises(Failure) as first:
                    lines.read()
                for operation in (lines.send, lines.call):
                    with patch("installed_guest.select.select") as ready, \
                            patch("installed_guest.os.write") as write, self.assertRaises(Failure) as again:
                        operation({"deadline_ns": 1}, deadline_ns=1)
                    self.assertIs(again.exception, first.exception)
                    self.assertEqual(again.exception.__notes__, ["bounded original evidence"])
                    ready.assert_not_called()
                    write.assert_not_called()
                with self.assertRaises(BlockingIOError):
                    os.read(read_fd, 1)
            self.assertEqual(os.read(read_fd, 1), b"")
        finally:
            os.close(read_fd)
            if write_fd is not None:
                os.close(write_fd)

        for mode in ("eof", "oversized", "broken-pipe", "live-timeout", "absolute-read-timeout",
                     "partial-write-timeout", "absolute-partial-write-timeout"):
            with self.subTest(mode=mode):
                input_read, input_write = os.pipe()
                output_read, output_write = os.pipe()
                with os.fdopen(input_read, "rb", buffering=0) as receiver, \
                        os.fdopen(input_write, "wb", buffering=0) as sender, \
                        os.fdopen(output_read, "rb", buffering=0) as responses, \
                        os.fdopen(output_write, "wb", buffering=0) as producer:
                    os.set_blocking(receiver.fileno(), False)
                    lines = Lines.__new__(Lines)
                    lines.child = Mock(stdin=sender, stdout=responses)
                    lines.buffer = bytearray()
                    lines.terminal_failure = None
                    if mode == "eof":
                        producer.close()
                    elif mode == "oversized":
                        lines.buffer = bytearray(b"x" * 1_048_576)
                        producer.write(b"x")
                    elif mode == "broken-pipe":
                        receiver.close()
                    caught = None
                    try:
                        if mode == "broken-pipe":
                            lines.send({"action": "snapshot"})
                        elif mode in ("live-timeout", "absolute-read-timeout"):
                            with patch("installed_guest.select.select", return_value=([], [], [])):
                                if mode == "live-timeout":
                                    lines.read(timeout=.001)
                                else:
                                    lines.read(deadline_ns=time.monotonic_ns() + 1_000_000_000)
                        elif mode in ("partial-write-timeout", "absolute-partial-write-timeout"):
                            original_write = os.write
                            with patch("installed_guest.select.select", side_effect=[([], [sender], []), ([], [], [])]), \
                                    patch("installed_guest.os.write", side_effect=lambda fd, data: original_write(fd, data[:1])) as partial:
                                budget = ({"deadline_ns": time.monotonic_ns() + 1_000_000_000}
                                          if mode.startswith("absolute-") else {"timeout": 1})
                                lines.send({"action": "snapshot"}, **budget)
                        else:
                            lines.read(timeout=.001)
                    except Exception as error:
                        caught = error
                    self.assertIsInstance(caught, Failure)
                    if mode in ("live-timeout", "absolute-read-timeout"):
                        self.assertIn("response timeout", str(caught))
                        producer.write(b'{"ok":true,"request_id":"late"}\n')
                    if "partial-write" in mode:
                        partial.assert_called_once()
                        self.assertEqual(os.read(receiver.fileno(), 4096), b"{")
                    self.assertIs(lines.terminal_failure, caught)
                    if "timeout" in mode:
                        self.assertEqual(caught.kind, "INVALID_OBSERVATION" if mode.startswith("absolute-") else "ABORTED_SAFETY")
                    if mode == "broken-pipe":
                        self.assertIsInstance(caught.__cause__, BrokenPipeError)
                    for operation in (lines.send, lines.call, lines.read):
                        with patch("installed_guest.select.select") as ready, \
                                patch("installed_guest.os.write") as write, patch("installed_guest.os.read") as read, \
                                self.assertRaises(Failure) as again:
                            if operation == lines.read:
                                operation(deadline_ns=1)
                            else:
                                operation({"action": "never-resend"}, deadline_ns=1)
                        self.assertIs(again.exception, caught)
                        ready.assert_not_called()
                        write.assert_not_called()
                        read.assert_not_called()
                    if mode != "broken-pipe":
                        with self.assertRaises(BlockingIOError):
                            os.read(receiver.fileno(), 1)
                        sender.close()
                        self.assertEqual(os.read(receiver.fileno(), 1), b"")

    def test_clipboard_reader_emits_terminal_first_failure_once(self):
        import io
        import types
        import installed_guest
        from gtk_target import ClipboardUnavailable
        watch = Mock()
        clipboard = Mock()
        target = types.SimpleNamespace(ClipboardPublicationWatch=Mock(return_value=watch),
            ClipboardUnavailable=ClipboardUnavailable, Gdk=Mock(), Gtk=Mock(),
            independent_clipboard_snapshot=Mock(side_effect=ValueError("first exact snapshot rejection")))
        target.Gtk.Clipboard.get.return_value = clipboard
        request = json.dumps({"deadline_ns": installed_guest.time.monotonic_ns() + 5_000_000_000}) + "\n"
        output = io.StringIO()
        with patch.dict(sys.modules, {"gtk_target": target}), patch("installed_guest.sys.stdin", io.StringIO(request * 2)), \
                patch("installed_guest.sys.stdout", output), patch("installed_guest.signal.signal"), \
                patch("installed_guest.signal.setitimer"):
            installed_guest.clipboard_reader_main()
        records = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual(len(records), 2)
        self.assertEqual(records[-1], {"ok": False, "terminal": True,
            "error": "first exact snapshot rejection", "kind": "INVALID_OBSERVATION"})
        target.independent_clipboard_snapshot.assert_called_once()
        watch.close.assert_called_once()

    def test_clipboard_reader_projects_only_known_numeric_notes_and_caches_first_terminal(self):
        import io
        import os
        import types
        import installed_guest
        from gtk_target import ClipboardUnavailable
        note = ("clipboard_fence before_owner=42 before_generation=2 before_timestamp=123 "
                "after_owner=42 after_generation=2 after_timestamp=123")
        for kind in ("known", "unknown-class", "invalid-note"):
            with self.subTest(kind=kind):
                error = (ValueError if kind == "unknown-class" else ClipboardUnavailable)(
                    "clipboard TIMESTAMP conversion unavailable")
                error.add_note(note if kind != "invalid-note" else note + " private-payload")
                error.add_note("private-payload " + "x" * 2048)
                watch = Mock()
                target = types.SimpleNamespace(ClipboardPublicationWatch=Mock(return_value=watch),
                    ClipboardUnavailable=ClipboardUnavailable, Gdk=Mock(), Gtk=Mock(),
                    independent_clipboard_snapshot=Mock(side_effect=error))
                request = json.dumps({"deadline_ns": installed_guest.time.monotonic_ns() + 5_000_000_000}) + "\n"
                output = io.StringIO()
                with patch.dict(sys.modules, {"gtk_target": target}), \
                        patch("installed_guest.sys.stdin", io.StringIO(request * 2)), \
                        patch("installed_guest.sys.stdout", output), patch("installed_guest.signal.signal"), \
                        patch("installed_guest.signal.setitimer"):
                    installed_guest.clipboard_reader_main()
                records = [json.loads(line) for line in output.getvalue().splitlines()]
                self.assertEqual(len(records), 2)
                response = records[-1]
                self.assertEqual(response["kind"], "INVALID_OBSERVATION")
                self.assertIs(response["terminal"], True)
                self.assertEqual(response.get("notes", []), [note] if kind == "known" else [])
                self.assertNotIn("private-payload", output.getvalue())
                target.independent_clipboard_snapshot.assert_called_once()
                watch.close.assert_called_once()
                read_fd, write_fd = os.pipe()
                try:
                    os.set_blocking(read_fd, False)
                    with os.fdopen(write_fd, "wb", buffering=0) as writer:
                        write_fd = None
                        channel = Lines.__new__(Lines)
                        channel.child = Mock(stdin=writer)
                        channel.buffer = bytearray(json.dumps(response).encode() + b"\n")
                        with self.assertRaises(Failure) as first:
                            channel.read()
                        self.assertEqual(getattr(first.exception, "__notes__", []),
                            [note] if kind == "known" else [])
                        for action in (channel.send, channel.call):
                            with patch("installed_guest.select.select") as ready, \
                                    patch("installed_guest.os.write") as write, self.assertRaises(Failure) as again:
                                action({}, deadline_ns=1)
                            self.assertIs(again.exception, first.exception)
                            ready.assert_not_called()
                            write.assert_not_called()
                        with self.assertRaises(BlockingIOError):
                            os.read(read_fd, 1)
                    self.assertEqual(os.read(read_fd, 1), b"")
                finally:
                    os.close(read_fd)
                    if write_fd is not None:
                        os.close(write_fd)

    def test_lifecycle_restore_dead_reader_uses_only_verified_pipeline_ack(self):
        import os
        from copy import deepcopy
        before, after = self.lifecycle_authority()
        first = Failure("original clipboard snapshot failure", "INVALID_OBSERVATION")
        ready = {"x11_health": "ready", "analysis_health": "ready", "input_health": "ready",
                 "learning_ready": 1, "learning_pending": 0, "learning_failed": 0,
                 "learning_completed_generation": 2, "learning_generation": 2,
                 "config_pending": 0, "config_result": "ok", "enabled": False}
        for mode in ("restored", "eof", "foreign-core", "unacknowledged-config"):
            with self.subTest(mode=mode):
                guest = Guest.__new__(Guest)
                guest.m = {"processes": {role: "/owned/" + role for role in after["identity"]}}
                phase = {"armed": True, "isolated": True, "current": before, "original": ["exact-original"], "original_enabled": False}
                guest.learning_phase = phase
                guest.clipboard_reader = Mock(terminal_failure=first)
                if mode == "eof":
                    read_fd, write_fd = os.pipe()
                    os.close(write_fd)
                    with os.fdopen(read_fd, "rb", buffering=0) as responses:
                        guest.clipboard_reader = Lines.__new__(Lines)
                        guest.clipboard_reader.child = Mock(stdout=responses)
                        guest.clipboard_reader.buffer = bytearray()
                        guest.clipboard_reader.terminal_failure = None
                        with self.assertRaises(Failure) as ended:
                            guest.clipboard_reader.read()
                        first = ended.exception
                        self.assertIs(guest.clipboard_reader.terminal_failure, first)
                    guest.clipboard_reader.call = Mock(side_effect=AssertionError("dead reader was resent"))
                guest.system = Mock(side_effect=[first, BrokenPipeError("secondary")])
                ids = deepcopy(after["identity"])
                if mode == "foreign-core":
                    ids["gnome"] = [999, "foreign"]
                steps = []
                guest.pipeline_admission = Mock(side_effect=lambda **_kwargs: (steps.append("pipeline") or ({}, ids)))
                guest.audio_close = lambda: steps.append("audio")
                guest.lifecycle_sound_close = lambda: steps.append("dialog")
                def stop():
                    steps.append("stop")
                    phase["old_dead"] = True
                guest.lifecycle_stop = stop
                guest.lifecycle_file = Mock(side_effect=lambda action: (steps.append(action) or
                    {"restored": True, "objects": phase["original"]}))
                guest.lifecycle_tray_identity = Mock()
                ack = {**ready, "config_pending": int(mode == "unacknowledged-config")}
                error, result = None, None
                with patch("installed_guest.checked_run", side_effect=lambda *_args, **_kw: steps.append("start")), \
                        patch("installed_guest.time.sleep"), patch("installed_guest.public_stats", side_effect=[ready, ack]), \
                        patch("installed_guest.runtime_status", side_effect=lambda *_args: steps.append("status")):
                    try:
                        result = guest.lifecycle_restore()
                    except Exception as caught:
                        error = caught
                if mode in ("restored", "eof"):
                    self.assertIsNone(error, str(error))
                    self.assertTrue(result["restoration_only"])
                    self.assertEqual(result["pipeline"], ids)
                    self.assertNotIn("after", result)
                    self.assertNotIn("clipboard", result)
                    self.assertEqual(result["restoration"]["objects"], ["exact-original"])
                    self.assertIsNone(guest.learning_phase)
                    self.assertEqual(steps, ["audio", "dialog", "stop", "restore", "start", "pipeline", "pipeline", "status"])
                    with tempfile.TemporaryDirectory() as directory:
                        runner = Collector.__new__(Collector)
                        runner.m = {"sources": {}, "package": {"path": "/owned/pkg", "sha256": "pinned"}}
                        runner.guard = Mock()
                        runner.output = Path(directory)
                        runner.rpc = Mock(return_value=result)
                        runner.authority = Mock()
                        runner.pipeline_successors = {"unchanged": [1, "original"]}
                        with patch("installed_guest.check_files"), self.assertRaises(Failure) as refused:
                            runner.lifecycle_rpc("restore")
                        self.assertEqual(refused.exception.kind, "INVALID_OBSERVATION")
                        runner.authority.assert_not_called()
                        self.assertEqual(runner.pipeline_successors, {"unchanged": [1, "original"]})
                        receipt = json.loads((Path(directory) / "lifecycle-transitions.jsonl").read_text())
                        self.assertTrue(receipt["restoration_only"])
                        self.assertNotIn("after", receipt)
                else:
                    self.assertIsInstance(error, Failure)
                    self.assertEqual(error.kind, "ABORTED_SAFETY")
                    self.assertIs(guest.learning_phase, phase)
                guest.system.assert_not_called()
                guest.clipboard_reader.call.assert_not_called()
                self.assertIs(guest.clipboard_reader.terminal_failure, first)

        # Initial startup admits the new writer chain before an observer rejects.
        guest = Guest.__new__(Guest)
        guest.m = {"installed": {}, "processes": {r: "/owned/" + r for r in after["identity"]},
                   "package": {"version": "2.8.14"}, "pipeline": {"evdev": "/owned/evdev"}}
        phase = {"armed": True, "old_dead": True, "current": before, "transition_from": before}
        guest.learning_phase = phase
        guest.lifecycle_tray_identity = Mock()
        guest.clipboard_reader = Mock()
        guest.clipboard_reader.call.side_effect = first
        def full_system(*_args, **_kwargs):
            return guest.clipboard_reader.call({})
        guest.system_observation = Mock(side_effect=full_system)
        with patch("installed_guest.check_files"), patch("installed_guest.checked_run", return_value="ii  2.8.14"), \
                patch("installed_guest.process_snapshot", return_value={}), \
                patch("installed_guest.process_identities", return_value=after["identity"].copy()), \
                patch("installed_guest.pipeline_facts"), self.assertRaises(Failure) as rejected:
            guest.lifecycle_start_pipeline()
        self.assertIs(rejected.exception, first)
        self.assertIs(phase["current"], before)
        self.assertEqual(phase["restoration_pipeline"], after["identity"])
        guest.clipboard_reader.call.assert_called_once()
        with patch("installed_guest.checked_run"), patch("installed_guest.Path") as proc:
            proc.return_value.exists.return_value = False
            Guest.lifecycle_stop(guest)
        self.assertEqual(proc.call_args_list, [call(
            "/proc", str(after["identity"][role][0]), "stat") for role in PIPELINE_ROLES])
        self.assertTrue(phase["old_dead"])

        # A failed full observation must not append observer roles to the cleanup ACK.
        admitted = deepcopy(after["identity"])
        frozen = deepcopy(admitted)
        observer = Guest.__new__(Guest)
        observer.m = {}
        observer.gtk = Mock(child=Mock(pid=222))
        observer.clipboard_reader = Mock(child=Mock(pid=333))
        observer.chrome = Mock(pid=444)
        observer.x = Mock()
        observer.x.XQueryKeymap.return_value = 0
        observer.display = None
        processes = {pid: ["owned", 0, "start"] for pid in (222, 333, 444)}
        with self.assertRaises(Failure):
            Guest.system_observation(observer, processes, admitted)
        self.assertEqual(admitted, frozen)

    def test_lifecycle_successors_are_exact_and_do_not_reset_baseline(self):
        from copy import deepcopy
        before, after = self.lifecycle_authority()
        successor_identity(before, after)
        runner = Collector.__new__(Collector)
        runner.m = {}
        runner.baseline = deepcopy(before)
        runner.pipeline_successors = {r: after["identity"][r] for r in PIPELINE_ROLES}
        runner.terminal_admission = None
        runner.last_group = 0
        runner.authority(after, foreground=True)
        self.assertEqual(runner.baseline, before)
        for role in after["identity"]:
            broken = deepcopy(after)
            broken["identity"][role] = [9999, "9"]
            if role not in PIPELINE_ROLES:
                with self.assertRaises(Failure):
                    successor_identity(before, broken)
                with self.assertRaises(Failure):
                    runner.authority(broken)
        for broken in (before, {**after, "clipboard": "foreign"}, {**after, "pressed": ["Ctrl"]},
                       {**after, "identity": {**after["identity"], "new": [99, "1"]}}):
            with self.assertRaises(Failure):
                successor_identity(before, broken)

    def lifecycle_fixture_snapshots(self):
        import base64
        from copy import deepcopy
        before, _ = self.lifecycle_authority()
        before["identity"].update(gtk_observer=[222, "gtk-start"], clipboard_reader=[333, "reader-start"])
        encoded = base64.b64encode(b"punto-g13-clipboard-preservation-sentinel").decode()
        values = {name: [kind, 8, encoded] for name, kind in (
            ("COMPOUND_TEXT", "COMPOUND_TEXT"), ("STRING", "STRING"), ("TEXT", "COMPOUND_TEXT"),
            ("UTF8_STRING", "UTF8_STRING"), ("text/plain", "text/plain"),
            ("text/plain;charset=utf-8", "text/plain;charset=utf-8"))}
        before["clipboard"] = {"owner": 11, "publication_generation": 7,
            "targets": "\n".join(sorted([*values, "TARGETS", "TIMESTAMP"])) + "\n",
            "representations": values, "bytes_base64": encoded}
        after = deepcopy(before)
        after["clipboard"].update(owner=22, publication_generation=8)
        proof = {"owner": 22, "timestamp": 123, "publication_generation": 1,
                 "pid": 222, "observer_instance_id": "owned-gtk"}
        return before, after, proof

    def test_lifecycle_fixture_profile_is_exact_before_any_publication(self):
        import installed_guest
        from copy import deepcopy
        validate = getattr(installed_guest, "lifecycle_fixture_profile", None)
        self.assertTrue(callable(validate), "fixed six-tuple fixture admission is absent")
        before, _, _ = self.lifecycle_fixture_snapshots()
        original = deepcopy(before["clipboard"])
        self.assertEqual(validate(original), "punto-g13-clipboard-preservation-sentinel")
        self.assertEqual(original, before["clipboard"])
        for fault in ("missing", "extra", "type", "format", "bytes", "default", "owner", "pending", "catalogue"):
            with self.subTest(fault=fault):
                value = deepcopy(original)
                if fault == "missing":
                    del value["representations"]["TEXT"]
                elif fault == "extra":
                    value["representations"]["text/html"] = ["text/html", 8, value["bytes_base64"]]
                elif fault in ("type", "format", "bytes"):
                    value["representations"]["TEXT"][("type", "format", "bytes").index(fault)] = (
                        "UTF8_STRING" if fault == "type" else 16 if fault == "format" else "Zm9yZWlnbg==")
                elif fault == "default":
                    value["bytes_base64"] = "Zm9yZWlnbg=="
                elif fault == "owner":
                    value["owner"] = 0
                elif fault == "pending":
                    value["publication_pending"] = True
                else:
                    value["targets"] += "text/html\n"
                frozen = deepcopy(value)
                with self.assertRaises(Failure):
                    validate(value)
                self.assertEqual(value, frozen)

    def test_lifecycle_fixture_guest_admits_once_and_never_retries_uncertain_write(self):
        import installed_guest
        from copy import deepcopy
        self.assertTrue(callable(getattr(Guest, "lifecycle_fixture", None)), "owned fixture action is absent")
        for fault in (None, "before-drift", "held", "missing", "identity", "group", "history", "owner", "payload", "late", "provider"):
            with self.subTest(fault=fault):
                before, after, proof = self.lifecycle_fixture_snapshots()
                original = deepcopy(before["clipboard"])
                if fault == "before-drift":
                    before["clipboard"]["owner"] += 1
                elif fault == "held":
                    before["pressed"] = ["Control_L"]
                elif fault == "missing":
                    del before["clipboard"]["representations"]["TEXT"]
                elif fault == "identity":
                    after["identity"]["helper"] = [999, "foreign"]
                elif fault == "group":
                    after["group"] ^= 1
                elif fault == "history":
                    after["clipboard"]["publication_generation"] = 7
                elif fault == "owner":
                    after["clipboard"]["owner"] = 99
                elif fault == "payload":
                    after["clipboard"]["representations"]["TEXT"][2] = "Zm9yZWlnbg=="
                guest = Guest.__new__(Guest)
                guest.m = {"lifecycle_phase": True}
                guest.learning_phase = None
                guest.surface = "gtk"
                guest.deadline_ns = installed_guest.time.monotonic_ns() + 5_000_000_000
                guest.system = Mock(side_effect=[before, after])
                identity = {"observer_instance_id": "owned-gtk"}
                first = Failure("first provider rejection", "INVALID_OBSERVATION")
                guest.target = Mock(side_effect=[identity, first if fault == "provider" else {"publication": proof}])
                guest.lifecycle_stop = Mock()
                real_system = guest.system
                late = [False]
                clock = installed_guest.time.monotonic_ns
                deadline = guest.deadline_ns
                if fault == "late":
                    def late_after(**_kwargs):
                        if real_system.call_count == 2:
                            late[0] = True
                            return after
                        return before
                    real_system.side_effect = late_after
                error, receipt = None, None
                with patch("installed_guest.time.monotonic_ns", side_effect=lambda: deadline + 1 if late[0] else clock()):
                    try:
                        receipt = guest.lifecycle_fixture(original)
                    except Exception as caught:
                        error = caught
                publications = [c for c in guest.target.call_args_list if c.args[0] == "lifecycle-fixture"]
                if fault is None:
                    self.assertIsNone(error, str(error))
                    self.assertEqual(receipt, {"before": before, "after": after, "publication": proof})
                    with self.assertRaises(Failure):
                        guest.lifecycle_fixture(original)
                    self.assertEqual(len([c for c in guest.target.call_args_list if c.args[0] == "lifecycle-fixture"]), 1)
                else:
                    self.assertIsInstance(error, Failure, str(error))
                    self.assertEqual(len(publications), int(fault not in ("before-drift", "held", "missing")))
                    if publications:
                        self.assertEqual(error.kind, "ABORTED_SAFETY")
                guest.lifecycle_stop.assert_not_called()
                self.assertEqual(original, self.lifecycle_fixture_snapshots()[0]["clipboard"])

    def test_lifecycle_fixture_collector_changes_only_proven_metadata(self):
        from copy import deepcopy
        self.assertTrue(callable(getattr(Collector, "lifecycle_fixture", None)), "fixture metadata admission is absent")
        for fault in (None, "before-drift", "missing", "identity", "history", "owner", "timestamp", "instance"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as directory:
                before, after, proof = self.lifecycle_fixture_snapshots()
                baseline = deepcopy(before)
                if fault == "before-drift":
                    before["clipboard"]["owner"] += 1
                elif fault == "missing":
                    del after["clipboard"]["representations"]["TEXT"]
                elif fault == "identity":
                    after["identity"]["daemon"] = [999, "foreign"]
                elif fault == "history":
                    after["clipboard"]["publication_generation"] = 7
                elif fault == "owner":
                    proof["owner"] = 99
                elif fault == "timestamp":
                    proof["timestamp"] = 0
                elif fault == "instance":
                    proof["pid"] = 999
                runner = Collector.__new__(Collector)
                runner.m = {"lifecycle_phase": True, "sources": {}, "package": {"path": "/owned/pkg", "sha256": "pinned"}}
                runner.output = Path(directory)
                runner.baseline = deepcopy(baseline)
                runner.pipeline_successors = {}
                runner.terminal_admission = None
                runner.last_group = before["group"]
                runner.guard = Mock()
                runner.rpc = Mock(return_value={"before": before, "after": after, "publication": proof})
                with patch("installed_guest.check_files"):
                    if fault is None:
                        runner.lifecycle_fixture()
                        self.assertEqual(runner.baseline, {**baseline, "clipboard": after["clipboard"]})
                        self.assertEqual(baseline["clipboard"], before["clipboard"])
                        self.assertEqual(runner.pipeline_successors, {})
                        runner.authority(after)
                        with self.assertRaises(Failure):
                            runner.lifecycle_fixture()
                        self.assertEqual(runner.rpc.call_count, 1)
                        record = json.loads((Path(directory) / "lifecycle-fixture.json").read_text())
                        self.assertEqual(record["before"], before)
                        self.assertEqual(record["after"], after)
                    else:
                        with self.assertRaises(Failure):
                            runner.lifecycle_fixture()
                        self.assertEqual(runner.baseline, baseline)
                        self.assertFalse((Path(directory) / "lifecycle-fixture.json").exists())

    def test_lifecycle_fixture_is_before_begin_without_republication_at_restart(self):
        runner = Collector.__new__(Collector)
        runner.m = {"lifecycle_phase": True}
        events = []
        runner.lifecycle_fixture = Mock(side_effect=lambda: events.append("fixture"))
        def begin(action, **_values):
            events.append(action)
            raise Failure("stop at begin boundary")
        runner.lifecycle_rpc = Mock(side_effect=begin)
        with self.assertRaises(Failure):
            runner.lifecycle()
        with self.subTest(boundary="begin-order"):
            self.assertEqual(events, ["fixture", "begin"])
            runner.lifecycle_fixture.assert_called_once()

        from copy import deepcopy
        for action in ("restart", "restore"):
            for fault in (None, "foreign", "missing"):
                with self.subTest(boundary=action, fault=fault):
                    before, _, _ = self.lifecycle_fixture_snapshots()
                    after = deepcopy(before)
                    for role in PIPELINE_ROLES:
                        after["identity"][role] = [after["identity"][role][0] + 100, "new-start"]
                    observed = deepcopy(before if action == "restart" else after)
                    if fault == "foreign":
                        observed["clipboard"]["owner"] = 99
                    elif fault == "missing":
                        del observed["clipboard"]["representations"]["TEXT"]
                    guest = Guest.__new__(Guest)
                    guest.m = {"processes": {role: "/owned/" + role for role in after["identity"]
                                            if role not in ("gtk_observer", "clipboard_reader")}}
                    phase = {"armed": True, "isolated": True, "current": before,
                             "original": ["exact-original"], "original_enabled": False}
                    guest.learning_phase = phase
                    guest.deadline_ns = None
                    guest.clipboard_reader = Mock(terminal_failure=None)
                    guest.require_owned_learning_phase = Mock(return_value={"store": ""})
                    guest.system = Mock(return_value=observed)
                    guest.system_observation = Mock(return_value=observed)
                    ids = {r: v for r, v in after["identity"].items() if r in guest.m["processes"]}
                    guest.pipeline_admission = Mock(return_value=({}, ids))
                    guest.target = Mock()
                    guest.lifecycle_fixture = Mock()
                    guest.audio_close = Mock()
                    guest.lifecycle_sound_close = Mock()
                    guest.lifecycle_tray_identity = Mock()
                    def stop():
                        phase["old_dead"] = True
                    guest.lifecycle_stop = Mock(side_effect=stop)
                    guest.lifecycle_file = Mock(return_value={"restored": True, "objects": phase["original"]})
                    ready = {"x11_health": "ready", "analysis_health": "ready", "input_health": "ready",
                        "learning_ready": 1, "learning_pending": 0, "learning_failed": 0,
                        "learning_completed_generation": 2, "learning_generation": 2,
                        "config_pending": 0, "config_result": "ok", "enabled": False}
                    with patch("installed_guest.checked_run"), patch("installed_guest.runtime_status"), \
                            patch("installed_guest.public_stats", return_value=ready):
                        if action == "restart":
                            guest.lifecycle_start_pipeline = Mock(return_value={"after": after})
                            if fault:
                                with self.assertRaises(Failure):
                                    guest.lifecycle_restart()
                                guest.lifecycle_stop.assert_not_called()
                                guest.lifecycle_start_pipeline.assert_not_called()
                            else:
                                guest.lifecycle_restart()
                                guest.lifecycle_stop.assert_called_once()
                        else:
                            receipt, error = None, None
                            try:
                                receipt = guest.lifecycle_restore()
                            except Exception as caught:
                                error = caught
                            self.assertIsNone(error, str(error))
                            guest.lifecycle_stop.assert_called_once()
                            guest.lifecycle_file.assert_called_once_with("restore")
                            if fault:
                                self.assertTrue(receipt["restoration_only"])
                                self.assertNotIn("after", receipt)
                                self.assertIn("observation_failure", receipt)
                                self.assertEqual(set(receipt["pipeline"]), set(guest.m["processes"]))
                            else:
                                self.assertEqual(receipt["after"], observed)
                            self.assertIsNone(guest.learning_phase)
                    guest.target.assert_not_called()
                    guest.lifecycle_fixture.assert_not_called()

    def test_lifecycle_fixture_target_checks_protocol_and_local_publication(self):
        import io, os, types
        import installed_guest
        from probe import ObservationProtocol
        for fault in (None, "active", "instance", "profile", "expired", "foreign", "destroyed", "xid", "timestamp", "history"):
            with self.subTest(fault=fault):
                before, _, _ = self.lifecycle_fixture_snapshots()
                created, steps = [], []
                class Scratch:
                    def __init__(self):
                        self.protocol = ObservationProtocol("gtk")
                        self.window = Mock()
                        created.append(self)
                    def handle_line(self, _line):
                        print(json.dumps({"ok": False, "error": "unsupported fixture action"}), flush=True)
                        return True
                display, window, clipboard, watch = Mock(), Mock(), Mock(), Mock()
                window.get_window_type.return_value = 99 if fault == "foreign" else 3
                window.get_display.return_value = display
                window.is_destroyed.return_value = fault == "destroyed"
                witness = {"owner": 22, "timestamp": 0 if fault == "timestamp" else 123, "publication_generation": 1}
                def fence(_deadline):
                    steps.append("fence")
                    return {"owner": 11, "timestamp": 122, "publication_generation": int(fault == "history")} if len(steps) == 1 else witness
                watch.fence.side_effect = fence
                clipboard.set_text.side_effect = lambda *_args: steps.append("publish")
                gdk = Mock()
                gdk.Display.get_default.return_value = display
                gdk.WindowType.FOREIGN = 99
                gtk = Mock()
                gtk.Clipboard.get.return_value = clipboard
                x11 = Mock()
                x11.X11Window.lookup_for_display.return_value = window
                x11.X11Window.get_xid.return_value = 23 if fault == "xid" else 22
                module = types.SimpleNamespace(ScratchTarget=Scratch, Gdk=gdk,
                    GLib=types.SimpleNamespace(IO_IN=1, IO_HUP=16, io_add_watch=Mock()), Gtk=gtk,
                    ClipboardPublicationWatch=Mock(return_value=watch))
                output = io.StringIO()
                with patch.dict(sys.modules, {"gtk_target": module, "gi": Mock(),
                                              "gi.repository": types.SimpleNamespace(GdkX11=x11)}), \
                        patch("installed_guest.sys.stdout", output):
                    installed_guest.gtk_main()
                    target = created[0]
                    if fault == "active":
                        target.protocol.run_id = "active-case"
                    deadline = installed_guest.time.monotonic_ns() + 5_000_000_000
                    command = {"action": "lifecycle-fixture", "clipboard": before["clipboard"],
                        "observer_instance_id": target.protocol.observer_instance_id, "deadline_ns": deadline}
                    if fault == "instance":
                        command["observer_instance_id"] = "foreign-instance"
                    elif fault == "profile":
                        del command["clipboard"]["representations"]["TEXT"]
                    elif fault == "expired":
                        command["deadline_ns"] = 1
                    target.handle_line(json.dumps(command))
                    response = json.loads(output.getvalue().splitlines()[-1])
                    if fault is None:
                        self.assertTrue(response["ok"], response)
                        self.assertEqual(response["publication"], {**witness, "pid": os.getpid(),
                            "observer_instance_id": target.protocol.observer_instance_id})
                        clipboard.set_text.assert_called_once_with("punto-g13-clipboard-preservation-sentinel", -1)
                        self.assertEqual(steps, ["fence", "publish", "fence"])
                        target.handle_line(json.dumps(command))
                        self.assertFalse(json.loads(output.getvalue().splitlines()[-1])["ok"])
                        self.assertEqual(clipboard.set_text.call_count, 1)
                    else:
                        self.assertFalse(response["ok"])
                        self.assertEqual(clipboard.set_text.call_count, int(fault in ("foreign", "destroyed", "xid", "timestamp", "history")))
                self.assertEqual(watch.close.call_count, int(fault in (None, "foreign", "destroyed", "xid", "timestamp", "history")))

    def test_lifecycle_durable_epoch_generation_and_count_are_mandatory(self):
        values = {"daemon_epoch": 7, "learning_ready": 1, "learning_pending": 0, "learning_failed": 0,
                  "learning_completed_generation": 4, "exclusions": 1}
        durable_learning(values, 7, 4, 1)
        for name, value in (("daemon_epoch", 8), ("learning_ready", 0), ("learning_pending", 1),
                            ("learning_failed", 1), ("learning_completed_generation", 3), ("exclusions", 0)):
            with self.subTest(name=name), self.assertRaises(Failure):
                durable_learning({**values, name: value}, 7, 4, 1)
        with self.assertRaises(Failure):
            durable_learning(values, 7, 0, 1)

    def test_lifecycle_canonical_pair_predeclared_hash_and_semantic_delta(self):
        import base64, hashlib
        on = b"auto_switch:\n  threshold: 3.5\nsound:\n  enabled: true"
        off = on.replace(b"enabled: true", b"enabled: false")
        spec = {"on": base64.b64encode(on).decode(), "off": base64.b64encode(off).decode(),
                "on_sha256": hashlib.sha256(on).hexdigest(), "off_sha256": hashlib.sha256(off).hexdigest()}
        original = {"auto_switch": {"threshold": 3.5}, "sound": {"enabled": True}}
        self.assertEqual(canonical_sound_pair(spec, original), {"on": on, "off": off})
        for broken in ({**spec, "off_sha256": "0" * 64}, {**spec, "extra": "calibration"}):
            with self.assertRaises(Failure):
                canonical_sound_pair(broken, original)
        for raw in (off.replace(b"3.5", b"4.5"), off.replace(b"false", b"0")):
            changed = {**spec, "off": base64.b64encode(raw).decode(), "off_sha256": hashlib.sha256(raw).hexdigest()}
            with self.assertRaises(Failure):
                canonical_sound_pair(changed, original)

    def test_lifecycle_owned_learning_boundary_fails_closed(self):
        guest = Guest.__new__(Guest)
        guest.learning_phase = None
        with patch("installed_guest.checked_run") as run, self.assertRaises(Failure):
            guest.require_owned_learning_phase()
        run.assert_not_called()

    def test_lifecycle_restore_stops_before_files_even_after_audio_cleanup_failure(self):
        before, after = self.lifecycle_authority()
        guest = Guest.__new__(Guest)
        guest.learning_phase = {"armed": True, "current": before, "original": ["original"], "original_enabled": False}
        steps = []
        def audio():
            steps.append("audio")
            raise RuntimeError("observer cleanup failure")
        guest.audio_close = audio
        guest.lifecycle_stop = lambda: steps.append("stop")
        guest.lifecycle_file = lambda action: (steps.append(action) or {"restored": True, "objects": ["original"]})
        guest.lifecycle_start_pipeline = lambda **_kwargs: (steps.append("start") or {
            "restoration_only": True, "pipeline": after["identity"], "stats": {}, "old_dead": True,
            "_readiness_deadline_ns": __import__("time").monotonic_ns() + 3_000_000_000})
        guest.pipeline_admission = Mock(return_value=({}, after["identity"]))
        guest.system_observation = Mock(return_value=after)
        with patch("installed_guest.runtime_status", side_effect=lambda *_: steps.append("status")), \
                patch("installed_guest.public_stats", side_effect=lambda *_: (steps.append("ack") or
                    {"config_pending": 0, "config_result": "ok", "enabled": False})), self.assertRaises(Failure) as error:
            guest.lifecycle_restore()
        self.assertEqual(steps, ["audio", "stop", "restore", "start", "status", "ack"])
        self.assertEqual(error.exception.kind, "ABORTED_SAFETY")
        self.assertIsNone(guest.learning_phase)

    def test_lifecycle_cleanup_executes_every_other_owned_callback(self):
        guest = Guest.__new__(Guest)
        guest.gtk, guest.chrome, guest.playwright = Mock(), Mock(), Mock()
        guest.clipboard_reader = Mock(spec=Lines)
        guest.chrome.poll.return_value = 0
        guest.lifecycle_restore = Mock(side_effect=Failure("restore failed", "ABORTED_SAFETY"))
        guest.terminal_close = Mock()
        with self.assertRaises(Failure) as error:
            guest.close()
        self.assertEqual(error.exception.kind, "ABORTED_SAFETY")
        guest.lifecycle_restore.assert_called_once()
        guest.terminal_close.assert_called_once()
        guest.gtk.close.assert_called_once()
        guest.clipboard_reader.close.assert_called_once()
        guest.playwright.stop.assert_called_once()

    def test_lifecycle_sound_writer_failure_stops_without_restoring_and_preserves_all_cleanup(self):
        for audio_failure in (False, True):
            with self.subTest(audio_failure=audio_failure):
                before, after = self.lifecycle_authority()
                phase = {"armed": True, "current": before, "original": ["private-backup"],
                         "original_enabled": False, "dialog_pending": True, "tray": {"identity": [42, "123"]}}
                guest = Guest.__new__(Guest)
                guest.learning_phase = phase
                guest.gtk, guest.chrome, guest.playwright = Mock(), Mock(), Mock()
                guest.clipboard_reader = Mock(spec=Lines)
                guest.chrome.poll.return_value = 0
                steps = []
                def audio():
                    steps.append("audio")
                    if audio_failure:
                        raise Failure("audio cleanup", "ABORTED_SAFETY")
                def visible(*_args, **_kwargs):
                    steps.append("dialog")
                    return subprocess.CompletedProcess([], 0, stdout=b"60\n61\n", stderr=b"")
                def stop():
                    steps.append("stop")
                    phase["old_dead"] = True
                guest.audio_close = audio
                guest.lifecycle_tray_identity = Mock()
                guest.lifecycle_stop = stop
                guest.lifecycle_file = Mock(side_effect=lambda action: (steps.append("files") or
                    {"restored": True, "objects": phase["original"]}))
                guest.lifecycle_start_pipeline = Mock(side_effect=lambda: (steps.append("restart") or {"after": after}))
                guest.terminal_close = Mock(side_effect=lambda: steps.append("terminal"))
                guest.gtk.close.side_effect = lambda: steps.append("gtk")
                guest.clipboard_reader.close.side_effect = lambda: steps.append("reader")
                guest.playwright.stop.side_effect = lambda: steps.append("playwright")
                with patch("installed_guest.subprocess.run", side_effect=visible), \
                        patch("installed_guest.runtime_status") as status, \
                        patch("installed_guest.public_stats", return_value={"config_pending": 0, "config_result": "ok", "enabled": False}) as stats, \
                        self.assertRaises(Failure) as error:
                    guest.close()
                self.assertEqual(error.exception.kind, "ABORTED_SAFETY")
                self.assertIs(guest.learning_phase, phase)
                self.assertTrue(phase["armed"] and phase["dialog_pending"] and phase["old_dead"])
                self.assertEqual(phase["original"], ["private-backup"])
                self.assertEqual(steps, ["audio", "dialog", "stop", "terminal", "gtk", "reader", "playwright"])
                guest.lifecycle_file.assert_not_called()
                guest.lifecycle_start_pipeline.assert_not_called()
                status.assert_not_called()
                stats.assert_not_called()

    def test_lifecycle_reset_uses_actual_documented_cli_not_second_raw_reset(self):
        guest = Guest.__new__(Guest)
        guest.learning_phase = {"boot": "boot-id"}
        before = {"stats": {"daemon_epoch": 7, "learning_generation": 2}}
        after = {"store": "", "stats": {"daemon_epoch": 7, "learning_generation": 3,
            "learning_ready": 1, "learning_pending": 0, "learning_failed": 0,
            "learning_completed_generation": 3, "exclusions": 0}}
        guest.lifecycle_stats = Mock(side_effect=[before, after])
        with patch("installed_guest.checked_run", side_effect=["OK EXCLUSIONS CLEARED\n", "PUNTO_RESET_V1 boot-id 123\n"]) as run:
            result = guest.lifecycle_reset()
        self.assertEqual(run.call_args_list[0].args[0], ["/usr/bin/punto", "reset-learning"])
        self.assertEqual(result["stats"]["learning_generation"], 3)
        self.assertIn("internal_epoch_ticket", result["ticket_proof"])
        self.assertNotIn("CLEAR_EXCLUSIONS", str(run.call_args_list))

    def test_lifecycle_public_stats_rejects_foreign_peer_and_partial_schema(self):
        client = Mock()
        client.__enter__ = Mock(return_value=client)
        client.__exit__ = Mock(return_value=False)
        client.getsockopt.return_value = __import__("struct").pack("3i", 99, 0, 0)
        with patch("installed_guest.socket.socket", return_value=client), self.assertRaises(Failure) as error:
            public_stats(42)
        self.assertEqual(error.exception.kind, "ABORTED_SAFETY")
        client.sendall.assert_not_called()
        client.getsockopt.return_value = __import__("struct").pack("3i", 42, 0, 0)
        client.recv.return_value = b"OK learning_ready=1\n"
        with patch("installed_guest.socket.socket", return_value=client), self.assertRaises(Failure):
            public_stats(42)

    def audio_fixture(self, enabled=True):
        import os
        window = {"id": "owned", "sequence": 1, "direction": 0, "enabled": enabled}
        player = {"pid": 41, "parent": 42, "uid": os.getuid(), "start": "10",
                  "argv": ["/usr/bin/paplay", "/usr/share/punto-switcher/sounds/en_ru.wav"],
                  "elf_sha256": "elf", "wave_sha256": "wave", "window": window}
        stream = {"player": player, "frames": 16, "nonzero": True, "window": window}
        spec = {"waves": {player["argv"][1]: "wave"}, "tools": {"/usr/bin/paplay": "elf"}}
        return window, player, stream, spec

    def test_lifecycle_audio_attribution_rejects_foreign_pid_elf_wave_and_parent(self):
        window, player, stream, spec = self.audio_fixture()
        validate_audio_player(player, [42, "9"], spec)
        for change in ({"parent": 43}, {"uid": player["uid"] + 1}, {"start": "0"},
                       {"elf_sha256": "other"}, {"wave_sha256": "other"},
                       {"argv": ["/usr/bin/paplay", "/tmp/foreign.wav"]}):
            with self.subTest(change=change), self.assertRaises(Failure):
                validate_audio_player({**player, **change}, [42, "9"], spec)

    def test_lifecycle_audio_includes_worker_thread_child_with_kernel_parent(self):
        import os
        import subprocess
        import threading
        ready, release = threading.Event(), threading.Event()
        owned = {}
        def worker():
            with subprocess.Popen(["/usr/bin/sleep", "5"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) as child:
                owned.update(pid=child.pid, tid=threading.get_native_id())
                ready.set()
                release.wait(3)
                child.terminate()
                child.wait(timeout=1)
        thread = threading.Thread(target=worker)
        thread.start()
        try:
            self.assertTrue(ready.wait(1))
            daemon = os.getpid()
            start = Path(f"/proc/{daemon}/stat").read_text().rsplit(")", 1)[1].split()[19]
            child = str(owned["pid"])
            self.assertNotIn(child, Path(f"/proc/{daemon}/task/{daemon}/children").read_text().split())
            self.assertIn(child, Path(f"/proc/{daemon}/task/{owned['tid']}/children").read_text().split())
            raw = Path(f"/proc/{child}/stat").read_text().rsplit(")", 1)[1].split()
            self.assertEqual(int(raw[1]), daemon)
            guest = Guest.__new__(Guest)
            guest.audio = {"daemon": [daemon, start]}
            self.assertIn(child, guest.audio_children())
            with patch("installed_guest.process_snapshot", return_value={11: ([], daemon, "1"), 12: ([], daemon + 1, "2")}):
                self.assertEqual(guest.audio_children(), ["11"])
        finally:
            release.set()
            thread.join(timeout=3)
        self.assertFalse(thread.is_alive())

    def test_lifecycle_audio_on_requires_exclusive_pcm_off_requires_same_live_observer(self):
        window, player, stream, spec = self.audio_fixture()
        validate_audio_window(window, [player], [stream], 2)
        for players, streams, sequence in (([], [stream], 2), ([player], [], 2), ([player], [stream], 1),
                ([player], [{**stream, "frames": 0}], 2), ([player], [{**stream, "nonzero": False}], 2),
                ([player], [{**stream, "player": {**player, "start": "11"}}], 2)):
            with self.subTest(sequence=sequence), self.assertRaises(Failure):
                validate_audio_window(window, players, streams, sequence)
        off = {**window, "enabled": False}
        validate_audio_window(off, [], [], 2)
        for players, streams, sequence in (([player], [], 2), ([], [stream], 2), ([], [], 1)):
            with self.assertRaises(Failure):
                validate_audio_window(off, players, streams, sequence)
        guest = Guest.__new__(Guest)
        for item in (None, {"error": "ambiguous stream", "ready": Mock(**{"is_set.return_value": True})}):
            guest.audio = item
            with self.assertRaises(Failure):
                guest.audio_health()

    def sandbox_files(self, root, action, **extra):
        import os, hashlib, base64
        config = root / "home/.config/punto/config.yaml"
        store = root / "etc/punto"
        source = LIFECYCLE_FILES.replace("/etc/punto", str(store)).replace("/home/punto/.config/punto/config.yaml", str(config))
        source = source.replace('for item in Path("/proc").glob("[0-9]*/exe"):', "for item in []:")
        source = source.replace('else 0)', 'else os.getuid())').replace('root.lstat().st_uid == 0', 'root.lstat().st_uid == os.getuid()')
        source = source.replace('(paths[0], b"", 0, 0)', '(paths[0], b"", os.getuid(), os.getgid())')
        value = {"action": action, "run_id": "00000000-0000-0000-0000-000000000001", "config": str(config),
                 "uid": os.getuid(), "gid": os.getgid(), "writers": [],
                 "off": base64.b64encode(b"owned-off").decode(),
                 "config_hashes": [hashlib.sha256(b"owned-off").hexdigest()], **extra}
        result = subprocess.run([sys.executable, "-c", source], input=json.dumps(value).encode(), capture_output=True, timeout=5)
        return result

    def lifecycle_file_fixture(self, root):
        import os
        store, home = root / "etc/punto", root / "home/.config/punto"
        store.mkdir(parents=True)
        home.mkdir(parents=True)
        paths = [store / "undo_exclusions.txt", store / ".undo_exclusions.txt.reset", home / "config.yaml"]
        for path, raw in zip(paths, (b"original\n", b"original-marker", b"original-config")):
            path.write_bytes(raw)
            path.chmod(0o600)
            os.utime(path, ns=(1_700_000_000_123456789, 1_700_000_001_987654321))
        os.setxattr(paths[2], "user.punto_acceptance", b"sealed-fixture")
        inspected = self.sandbox_files(root, "inspect")
        self.assertEqual(inspected.returncode, 0, inspected.stderr)
        return paths, json.loads(inspected.stdout)["objects"]

    def test_lifecycle_system_uses_only_established_pair_and_preserves_other_pins(self):
        import hashlib
        for state in ("before", "isolated", "restored", "unarmed"):
            for raw in (b"original-config", b"owned-on", b"owned-off", b"foreign-config"):
                with self.subTest(state=state, raw=raw), tempfile.TemporaryDirectory(dir=Path(__file__).parent) as name:
                    root = Path(name)
                    paths, _ = self.lifecycle_file_fixture(root)
                    cfg = paths[2]
                    other = root / "installed-payload"
                    other.write_bytes(b"pinned-payload")
                    installed = {str(cfg): hashlib.sha256(b"original-config").hexdigest(),
                                 str(other): hashlib.sha256(b"pinned-payload").hexdigest()}
                    expected_installed = installed.copy()
                    cfg.write_bytes(raw)
                    guest = Guest.__new__(Guest)
                    guest.m = {"installed": installed, "package": {"version": "2.8.14"}}
                    guest.deadline_ns = None
                    guest.learning_phase = {"armed": state != "unarmed", "isolated": state in ("isolated", "unarmed"),
                        "config": str(cfg), "pair": {"on": b"owned-on", "off": b"owned-off"}}
                    guest.require_owned_learning_phase = Mock(return_value={"store": ""})
                    def inspect(action):
                        self.assertEqual(action, "inspect")
                        value = self.sandbox_files(root, action)
                        if value.returncode:
                            raise Failure("fixed-file inspection refused")
                        return json.loads(value.stdout)
                    guest.lifecycle_file = inspect
                    reached = RuntimeError("post-payload boundary reached")
                    with patch("installed_guest.checked_run", side_effect=reached) as command:
                        try:
                            guest.system(prepare_pending=True)
                        except Exception as error:
                            admitted = raw in (b"owned-on", b"owned-off") if state == "isolated" else raw == b"original-config"
                            if admitted:
                                self.assertIs(error, reached)
                                command.assert_called_once()
                            else:
                                self.assertIsInstance(error, Failure)
                                command.assert_not_called()
                        else:
                            self.fail("system bypassed the post-payload boundary")
                    self.assertEqual(guest.m["installed"], expected_installed)
                    self.assertEqual(installed, expected_installed)

    def test_lifecycle_system_refuses_foreign_pair_metadata_guard_pins_and_late_reads(self):
        import hashlib, os
        for fault in ("uid", "gid", "mode", "symlink", "hardlink", "absence", "other-pin", "guard", "expired", "late"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory(dir=Path(__file__).parent) as name:
                root = Path(name)
                paths, _ = self.lifecycle_file_fixture(root)
                cfg = paths[2]
                cfg.write_bytes(b"owned-off")
                other = root / "installed-payload"
                other.write_bytes(b"pinned-payload")
                installed = {str(cfg): hashlib.sha256(b"original-config").hexdigest(),
                             str(other): hashlib.sha256(b"pinned-payload").hexdigest()}
                expected_installed = installed.copy()
                guest = Guest.__new__(Guest)
                guest.m = {"installed": installed, "package": {"version": "2.8.14"}}
                guest.deadline_ns = 5 if fault in ("expired", "late") else None
                guest.learning_phase = {"armed": True, "isolated": True, "config": str(cfg),
                                        "pair": {"on": b"owned-on", "off": b"owned-off"}}
                guest.require_owned_learning_phase = Mock(return_value={"store": ""})
                if fault == "guard":
                    guest.require_owned_learning_phase.side_effect = Failure("learning owner refused")
                elif fault == "mode":
                    cfg.chmod(0o644)
                elif fault == "symlink":
                    cfg.unlink()
                    cfg.symlink_to(other)
                elif fault == "hardlink":
                    os.link(cfg, root / "foreign-link")
                elif fault == "absence":
                    cfg.unlink()
                elif fault == "other-pin":
                    other.write_bytes(b"foreign-payload")
                def inspect(action):
                    self.assertEqual(action, "inspect")
                    value = self.sandbox_files(root, action)
                    if value.returncode:
                        raise Failure("fixed-file inspection refused")
                    record = json.loads(value.stdout)
                    if fault in ("uid", "gid"):
                        record["objects"][2][fault] += 1
                    return record
                guest.lifecycle_file = Mock(side_effect=inspect)
                ticks = [9, 9] if fault == "expired" else [1, 9] if fault == "late" else [1, 1]
                with patch("installed_guest.time.monotonic_ns", side_effect=ticks), \
                        patch("installed_guest.checked_run") as command, self.assertRaises(Failure):
                    guest.system(prepare_pending=True)
                command.assert_not_called()
                self.assertEqual(guest.m["installed"], expected_installed)
                self.assertEqual(installed, expected_installed)
                if fault == "expired":
                    guest.lifecycle_file.assert_not_called()

    def test_lifecycle_isolated_handoff_is_after_backup_and_before_successor(self):
        import base64, hashlib
        before, after = self.lifecycle_authority()
        guest = Guest.__new__(Guest)
        original = [{}, {}, {"sha256": "original"}]
        guest.m = {"lifecycle_phase": True, "session": {"HOME": "/home/punto"},
                   "lifecycle": {"original_config_sha256": "original", "config_pair": {}}}
        guest.learning_phase = None
        guest.clipboard_reader = Mock(spec=Lines)
        def files(action):
            self.assertFalse(guest.learning_phase.get("isolated", False))
            if action == "inspect":
                return {"objects": original, "config": base64.b64encode(b"sound:\n  enabled: false").decode()}
            self.assertEqual(action, "isolate")
            return {"objects": original, "isolated": [{"sha256": hashlib.sha256(b"").hexdigest()}, {}, {}]}
        guest.lifecycle_file = files
        guest.system = Mock(return_value=before)
        guest.lifecycle_tray_identity = Mock(return_value={})
        guest.lifecycle_stop = Mock()
        guest.require_owned_learning_phase = Mock(return_value={"store": ""})
        guest.lifecycle_restore = Mock()
        def start():
            self.assertTrue(guest.learning_phase.get("isolated", False))
            return {"after": after, "stats": {"exclusions": 0}}
        guest.lifecycle_start_pipeline = start
        with patch("installed_guest.canonical_sound_pair", return_value={"on": b"owned-on", "off": b"owned-off"}), \
                patch("installed_guest.runtime_status", return_value=False), \
                patch("installed_guest.Path.read_text", return_value="owned-boot"):
            guest.lifecycle_begin()
        for failed in (True, False):
            phase = {"armed": True, "isolated": True, "current": before, "original": original, "original_enabled": False}
            guest.learning_phase = phase
            guest.audio_close, guest.lifecycle_sound_close = Mock(), Mock()
            guest.lifecycle_file = Mock(side_effect=Failure("restore refused") if failed else None,
                                        return_value={"restored": True, "objects": original})
            def restored_start(**_kwargs):
                self.assertFalse(phase["isolated"])
                return {"restoration_only": True, "pipeline": after["identity"], "stats": {}, "old_dead": True,
                        "_readiness_deadline_ns": __import__("time").monotonic_ns() + 3_000_000_000}
            guest.lifecycle_start_pipeline = Mock(side_effect=restored_start)
            guest.pipeline_admission = Mock(return_value=({}, after["identity"]))
            guest.system_observation = Mock(return_value=after)
            with patch("installed_guest.runtime_status"), patch("installed_guest.public_stats", return_value={
                    "config_pending": 0, "config_result": "ok", "enabled": False}):
                if failed:
                    with self.assertRaises(Failure):
                        Guest.lifecycle_restore(guest)
                    self.assertTrue(phase["isolated"])
                    self.assertTrue(phase["armed"])
                    guest.lifecycle_start_pipeline.assert_not_called()
                else:
                    Guest.lifecycle_restore(guest)
                    guest.lifecycle_start_pipeline.assert_called_once()
                    self.assertIsNone(guest.learning_phase)

    def test_lifecycle_scratch_gid_refusal_precedes_any_restore_mutation(self):
        import os
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as name:
            root = Path(name)
            paths, original = self.lifecycle_file_fixture(root)
            self.assertEqual(self.sandbox_files(root, "isolate", original=original).returncode, 0)
            before = [(path.lstat().st_ino, path.lstat().st_ctime_ns) if path.exists() else None for path in paths]
            result = self.sandbox_files(root, "restore", original=original, gid=os.getgid() + 1)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual([(path.lstat().st_ino, path.lstat().st_ctime_ns) if path.exists() else None for path in paths], before)

    def test_lifecycle_host_cleanup_notes_preserve_both_failures_in_actual_summary(self):
        import installed_guest
        for has_primary in (True, False):
            with self.subTest(has_primary=has_primary), tempfile.TemporaryDirectory(dir=Path(__file__).parent) as name:
                root = Path(name)
                (root / "control-output").mkdir()
                primary = Failure("host primary refusal", "FAIL_PRODUCT")
                cleanup = Failure("host cleanup refusal", "ABORTED_SAFETY")
                for error in (primary, cleanup):
                    for _ in range(10):
                        error.add_note("retained-diagnostic-" + "x" * 2048)
                collector = Collector({"lifecycle_phase": True}, Mock(), Mock(), root / "control-output")
                collector.lifecycle_word, collector.operation, collector.rpc = Mock(), Mock(), Mock()
                collector.lifecycle_fixture = Mock()
                def lifecycle_rpc(action, **_fields):
                    if action == "reset" and has_primary:
                        raise primary
                    if action == "restore":
                        raise cleanup
                    return {"store": ""} if action == "stats" else {}
                collector.lifecycle_rpc = Mock(side_effect=lifecycle_rpc)
                with self.assertRaises(Failure) as raised:
                    collector.lifecycle()
                error = raised.exception
                self.assertEqual(error.kind, "ABORTED_SAFETY")
                self.assertIs(error.__cause__, primary if has_primary else cleanup)
                notes = getattr(error, "__notes__", [])
                self.assertTrue(any("host cleanup refusal" in note and "ABORTED_SAFETY" in note for note in notes))
                if has_primary:
                    self.assertTrue(any("host primary refusal" in note and "FAIL_PRODUCT" in note for note in notes))
                self.assertLessEqual(len(notes), 8)
                self.assertTrue(all(len(note) <= 1024 for note in notes))
                self.assertTrue(any("retained-diagnostic-" in note for note in notes))
                self.assertEqual(sum(call.args == ("restore",) for call in collector.lifecycle_rpc.call_args_list), 1)
                collector.lifecycle_rpc.reset_mock()
                for filename in ("key", "known_hosts"):
                    (root / filename).touch()
                manifest = {"domain_uuid": "owned-domain", "ssh_host": "punto@192.0.2.8",
                    "sources": {"owned": "hash"}, "installed": {"owned": "hash"},
                    "processes": {role: "/owned/" + role for role in (*PIPELINE_ROLES, "helper", "gnome", "ibus")},
                    "session": {"XDG_SESSION_TYPE": "x11"}, "staged_dir": "/home/punto/punto-acceptance-host-notes",
                    "package": {"path": "/owned/package", "sha256": "hash"}, "python": sys.executable}
                capability = self.phase_capability_manifest()
                manifest["lifecycle"] = capability["lifecycle"]
                manifest["processes"].update(capability["processes"])
                manifest["installed"].update(capability["installed"])
                manifest_path = root / "manifest.json"
                manifest_path.write_text(json.dumps(manifest))
                args = ["installed_guest.py", "--vm", "owned", "--uuid", "owned-domain", "--host", manifest["ssh_host"],
                    "--key", str(root / "key"), "--known-hosts", str(root / "known_hosts"),
                    "--expected-payload-manifest", str(manifest_path), "--output", str(root / "output"), "--lifecycle"]
                with patch("installed_guest.sys.argv", args), patch("installed_guest.Guard"), \
                        patch("installed_guest.check_files"), patch("installed_guest.checked_run"), \
                        patch("installed_guest.Collector", return_value=collector), \
                        patch("installed_guest.Lines", return_value=Mock(**{"read.return_value": {"ready": True}})):
                    self.assertEqual(installed_guest.main(), 1)
                summary = json.loads((root / "output/summary.json").read_text())
                self.assertEqual(summary["status"], "ABORTED_SAFETY")
                self.assertEqual(summary["notes"], notes)
                self.assertEqual(sum(call.args == ("restore",) for call in collector.lifecycle_rpc.call_args_list), 1)

    def test_lifecycle_access_time_reads_preserve_inspected_originals_and_idempotent_restore(self):
        import os
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as name:
            root = Path(name)
            paths, original = self.lifecycle_file_fixture(root)
            for path, record in zip(paths, original):
                os.utime(path, ns=(record["atime_ns"] + 1_000_000_000, record["mtime_ns"]))
            isolated = self.sandbox_files(root, "isolate", original=original)
            self.assertEqual(isolated.returncode, 0, isolated.stderr)
            self.assertEqual(json.loads(isolated.stdout)["objects"], original)
            backup = root / "etc/punto/.acceptance-00000000-0000-0000-0000-000000000001"
            self.assertEqual(json.loads((backup / "originals.json").read_text())["objects"], original)
            restored = self.sandbox_files(root, "restore", original=original)
            self.assertEqual(restored.returncode, 0, restored.stderr)
            self.assertEqual(json.loads(restored.stdout)["objects"], original)
            for path, record in zip(paths, original):
                self.assertEqual((path.stat().st_atime_ns, path.stat().st_mtime_ns),
                                 (record["atime_ns"], record["mtime_ns"]))
                os.utime(path, ns=(record["atime_ns"] + 2_000_000_000, record["mtime_ns"]))
            repeated = self.sandbox_files(root, "restore", original=original)
            self.assertEqual(repeated.returncode, 0, repeated.stderr)
            self.assertEqual(json.loads(repeated.stdout)["objects"], original)
            for path, record in zip(paths, original):
                self.assertEqual((path.stat().st_atime_ns, path.stat().st_mtime_ns),
                                 (record["atime_ns"], record["mtime_ns"]))

    def test_lifecycle_all_originals_are_admitted_before_any_isolation_mutation(self):
        import copy, os
        for change in ("sha256", "uid", "gid", "mode", "mtime_ns", "xattrs", "symlink", "hardlink", "absence"):
            with self.subTest(change=change), tempfile.TemporaryDirectory(dir=Path(__file__).parent) as name:
                root = Path(name)
                paths, original = self.lifecycle_file_fixture(root)
                expected = copy.deepcopy(original)
                cfg = paths[2]
                if change in ("uid", "gid"):
                    expected[2][change] += 1
                elif change == "sha256":
                    cfg.write_bytes(b"foreign-config")
                elif change == "mode":
                    cfg.chmod(0o644)
                elif change == "mtime_ns":
                    os.utime(cfg, ns=(original[2]["atime_ns"], original[2]["mtime_ns"] + 1))
                elif change == "xattrs":
                    os.setxattr(cfg, "user.punto_acceptance", b"foreign")
                elif change == "symlink":
                    cfg.unlink()
                    cfg.symlink_to(paths[0])
                elif change == "hardlink":
                    os.link(cfg, root / "foreign-link")
                else:
                    cfg.unlink()
                before = [path.lstat() if path.exists() or path.is_symlink() else None for path in paths]
                result = self.sandbox_files(root, "isolate", original=expected)
                self.assertNotEqual(result.returncode, 0, "foreign original admitted: " + change)
                self.assertFalse((root / "etc/punto/.acceptance-00000000-0000-0000-0000-000000000001").exists())
                self.assertEqual([path.lstat() if path.exists() or path.is_symlink() else None for path in paths], before)

    def test_lifecycle_restore_admits_every_original_before_timestamp_or_file_mutation(self):
        import os
        for backup_present in (False, True):
            for change in ("sha256", "mode", "mtime_ns", "xattrs", "symlink", "hardlink", "absence"):
                with self.subTest(backup_present=backup_present, change=change), \
                        tempfile.TemporaryDirectory(dir=Path(__file__).parent) as name:
                    root = Path(name)
                    paths, original = self.lifecycle_file_fixture(root)
                    isolated = self.sandbox_files(root, "isolate", original=original)
                    self.assertEqual(isolated.returncode, 0, isolated.stderr)
                    backup = root / "etc/punto/.acceptance-00000000-0000-0000-0000-000000000001"
                    if not backup_present:
                        restored = self.sandbox_files(root, "restore", original=original)
                        self.assertEqual(restored.returncode, 0, restored.stderr)
                    cfg = backup / "2" if backup_present else paths[2]
                    if change == "sha256":
                        cfg.write_bytes(b"foreign-config")
                    elif change == "mode":
                        cfg.chmod(0o644)
                    elif change == "mtime_ns":
                        os.utime(cfg, ns=(original[2]["atime_ns"], original[2]["mtime_ns"] + 1))
                    elif change == "xattrs":
                        os.setxattr(cfg, "user.punto_acceptance", b"foreign")
                    elif change == "symlink":
                        cfg.unlink()
                        cfg.symlink_to(paths[0])
                    elif change == "hardlink":
                        os.link(cfg, root / "foreign-link")
                    else:
                        cfg.unlink()
                    # First two objects would otherwise be processed before the invalid config.
                    def stable_stat(path):
                        try:
                            value = path.lstat()
                        except FileNotFoundError:
                            return None
                        return (value.st_dev, value.st_ino, value.st_mode, value.st_uid, value.st_gid,
                                value.st_nlink, value.st_size, value.st_mtime_ns, value.st_ctime_ns)
                    before = {str(path): stable_stat(path) for path in [*paths, *backup.iterdir()]}
                    result = self.sandbox_files(root, "restore", original=original)
                    self.assertNotEqual(result.returncode, 0, "foreign restore admitted: " + change)
                    self.assertEqual({key: stable_stat(Path(key)) for key in before}, before)

    def test_lifecycle_primary_and_cleanup_notes_cross_guest_lines_and_summary(self):
        import base64, io, installed_guest
        before, _ = self.lifecycle_authority()
        guest = Guest.__new__(Guest)
        guest.m = {"lifecycle_phase": True, "session": {"HOME": "/home/punto"},
                   "lifecycle": {"original_config_sha256": "original", "config_pair": {}}}
        guest.learning_phase = None
        guest.clipboard_reader = Mock(spec=Lines)
        primary = Failure("primary isolation refused", "BLOCKED_PRECONDITION")
        cleanup = Failure("cleanup metadata refused", "ABORTED_SAFETY")
        guest.lifecycle_file = Mock(side_effect=[{"objects": [{}, {}, {"sha256": "original"}],
            "config": base64.b64encode(b"sound:\n  enabled: false").decode()}, primary])
        guest.system = Mock(return_value=before)
        guest.lifecycle_tray_identity = Mock(return_value={})
        guest.lifecycle_stop = Mock()
        guest.lifecycle_restore = Mock(side_effect=cleanup)
        with patch("installed_guest.canonical_sound_pair", return_value={}), \
                patch("installed_guest.runtime_status", return_value=False), \
                patch("installed_guest.Path.read_text", return_value="owned-boot"), self.assertRaises(Failure) as raised:
            guest.lifecycle_begin()
        failure = raised.exception
        self.assertEqual(failure.kind, "ABORTED_SAFETY")
        self.assertTrue(any("primary isolation refused" in note for note in getattr(failure, "__notes__", [])))
        self.assertTrue(any("cleanup metadata refused" in note for note in getattr(failure, "__notes__", [])))
        for _ in range(10):
            failure.add_note("oversized-note-" + "x" * 2048)
        expected_notes = [note[:1024] for note in failure.__notes__[:8]]
        projected_guest = Mock()
        projected_guest.call.side_effect = failure
        manifest = {"domain_uuid": "owned-domain", "session": {}, "observer_assets": {}, "chromium": "chromium"}
        request = {"action": "lifecycle-begin"}
        with patch("installed_guest.guest_group", return_value=42), patch("installed_guest.Guest", return_value=projected_guest), \
                patch("installed_guest.checked_run", return_value="owned-domain"), patch("installed_guest.check_files"), \
                patch("installed_guest.signal.signal"), patch("installed_guest.signal.setitimer"), \
                patch("installed_guest.sys.stdin", io.StringIO(json.dumps(manifest) + "\n" + json.dumps(request) + "\n")), \
                patch("builtins.print") as output:
            installed_guest.guest_main()
        response = json.loads(output.call_args.args[0])
        self.assertEqual(response["notes"], expected_notes)
        self.assertEqual(len(response["notes"]), 8)
        self.assertTrue(all(len(note) <= 1024 for note in response["notes"]))
        # Lines also bounds a malformed-sized but otherwise owned guest note packet.
        response["notes"] = failure.__notes__
        lines = Lines.__new__(Lines)
        lines.buffer = bytearray((json.dumps(response) + "\n").encode())
        lines.child, lines.send, lines.close = Mock(), Mock(), Mock()
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as name:
            root = Path(name)
            for filename in ("key", "known_hosts"):
                (root / filename).touch()
            manifest.update(ssh_host="punto@192.0.2.8", sources={"owned": "hash"}, installed={"owned": "hash"},
                processes={role: "/owned/" + role for role in (*PIPELINE_ROLES, "helper", "gnome", "ibus")},
                session={"XDG_SESSION_TYPE": "x11"}, staged_dir="/home/punto/punto-acceptance-notes",
                package={"path": "/owned/package", "sha256": "hash"}, python=sys.executable)
            manifest_path = root / "manifest.json"
            manifest_path.write_text(json.dumps(manifest))
            args = ["installed_guest.py", "--vm", "owned", "--uuid", "owned-domain", "--host", manifest["ssh_host"],
                    "--key", str(root / "key"), "--known-hosts", str(root / "known_hosts"),
                    "--expected-payload-manifest", str(manifest_path), "--output", str(root / "output")]
            with patch("installed_guest.sys.argv", args), patch("installed_guest.Guard"), \
                    patch("installed_guest.check_files"), patch("installed_guest.checked_run"), \
                    patch("installed_guest.Lines", return_value=lines):
                self.assertEqual(installed_guest.main(), 1)
            summary = json.loads((root / "output/summary.json").read_text())
        self.assertEqual(summary["status"], "ABORTED_SAFETY")
        self.assertEqual(summary["notes"], expected_notes)

    def test_lifecycle_real_file_backup_restore_bytes_metadata_and_absence(self):
        import os, base64, hashlib
        # The privilege/process boundary is mocked; file operations use only cache-local scratch paths.
        for absent in (False, True):
            with self.subTest(absent=absent), tempfile.TemporaryDirectory(dir=Path(__file__).parent) as name:
                root = Path(name)
                store, home = root / "etc/punto", root / "home/.config"
                store.mkdir(parents=True)
                home.mkdir(parents=True)
                cfg = home / "punto/config.yaml"
                paths = [store / "undo_exclusions.txt", store / ".undo_exclusions.txt.reset", cfg]
                private_value = b"private-owned-fixture-attribute"
                if not absent:
                    cfg.parent.mkdir()
                    for path, raw in zip(paths, (b"original\n", b"original-marker", b"original-config")):
                        path.write_bytes(raw)
                        path.chmod(0o600)
                    os.setxattr(paths[0], "user.punto_acceptance", private_value)
                initial = self.sandbox_files(root, "inspect")
                # Absence inspect reads the normal system config fallback.
                if absent:
                    (store / "config.yaml").write_bytes(b"system-config")
                    initial = self.sandbox_files(root, "inspect")
                self.assertEqual(initial.returncode, 0, initial.stderr)
                original = json.loads(initial.stdout)["objects"]
                if not absent:
                    self.assertEqual(original[0]["xattrs"]["user.punto_acceptance"], hashlib.sha256(private_value).hexdigest())
                isolated = self.sandbox_files(root, "isolate", original=original)
                self.assertEqual(isolated.returncode, 0, isolated.stderr)
                self.assertEqual(paths[0].read_bytes(), b"")
                restored = self.sandbox_files(root, "restore", original=original)
                self.assertEqual(restored.returncode, 0, restored.stderr)
                self.assertEqual(json.loads(restored.stdout)["objects"], original)
                for result in (initial, isolated, restored):
                    self.assertNotIn(private_value, result.stdout)
                    self.assertNotIn(base64.b64encode(private_value), result.stdout)
                for path, metadata in zip(paths, original):
                    self.assertEqual(path.exists(), not metadata.get("absent", False))
                if absent:
                    self.assertFalse(cfg.parent.exists())
                else:
                    self.assertEqual(os.getxattr(paths[0], "user.punto_acceptance"), private_value)

    def test_lifecycle_file_safety_preserves_backup_when_foreign_config_appears(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as name:
            root = Path(name)
            store, home = root / "etc/punto", root / "home/.config/punto"
            store.mkdir(parents=True)
            home.mkdir(parents=True)
            (store / "config.yaml").write_bytes(b"system-config")
            cfg = home / "config.yaml"
            cfg.write_bytes(b"original-config")
            cfg.chmod(0o600)
            original = json.loads(self.sandbox_files(root, "inspect").stdout)["objects"]
            self.assertEqual(self.sandbox_files(root, "isolate", original=original).returncode, 0)
            cfg.write_bytes(b"foreign-concurrent-change")
            result = self.sandbox_files(root, "restore", original=original)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(cfg.read_bytes(), b"foreign-concurrent-change")
            backup = store / ".acceptance-00000000-0000-0000-0000-000000000001/2"
            self.assertEqual(backup.read_bytes(), b"original-config")

    def test_lifecycle_file_safety_rejects_symlink_and_hardlink_before_isolation(self):
        for kind in ("symlink", "hardlink"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory(dir=Path(__file__).parent) as name:
                root = Path(name)
                store, home = root / "etc/punto", root / "home/.config/punto"
                store.mkdir(parents=True)
                home.mkdir(parents=True)
                target = store / "config.yaml"
                target.write_bytes(b"system-config")
                cfg = home / "config.yaml"
                if kind == "symlink":
                    cfg.symlink_to(target)
                else:
                    __import__("os").link(target, cfg)
                result = self.sandbox_files(root, "inspect")
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((store / ".acceptance-00000000-0000-0000-0000-000000000001").exists())


    def sound_recipe(self, fault=None):
        from types import SimpleNamespace
        on = b"sound:\n  enabled: true"
        off = b"sound:\n  enabled: false"
        flag = {"enabled": False, "generation": 0, "closed": False, "calls": [], "clock": 0, "searches": [],
                "full_stats": 0, "public_stats": []}
        def clock():
            flag["clock"] += .005
            return flag["clock"]
        def node(name, children=(), pid=42):
            value = Mock()
            value.get_name.return_value = name
            value.get_process_id.return_value = pid
            value.get_child_count.return_value = len(children)
            value.get_child_at_index.side_effect = lambda i: children[i]
            return value
        checkbox, save = node("Звук при смене раскладки"), node("Сохранить")
        dialog = node("Звук исправлений Punto Switcher", [checkbox, save])
        app = node("Punto", [dialog], 43 if fault == "pid" else 42)
        desktop = node("desktop", [app])
        for current in (checkbox, save):
            state = Mock()
            state.contains.side_effect = lambda kind, current=current: (
                flag["enabled"] if current is checkbox and kind == 1 else
                fault == "initial-sensitive" or (flag["enabled"] and fault != "dirty-insensitive")
                if current is save and kind == 2 else False)
            current.get_state_set.return_value = state
        atspi = Mock()
        atspi.get_desktop.return_value = desktop
        atspi.StateType = SimpleNamespace(CHECKED=1, SENSITIVE=2)
        bus = Mock()
        def call(_owner, _path, _interface, method, *_):
            flag["calls"].append(method)
            value = Mock()
            value.unpack.return_value = (2, (0, {}, [(3, {"label": "Звук исправлений..."}, [])])) if method == "GetLayout" else ()
            return value
        bus.call_sync.side_effect = call
        gio, glib, gi = Mock(), Mock(), Mock()
        gio.bus_get_sync.return_value = bus
        repository = SimpleNamespace(Atspi=atspi, Gio=gio, GLib=glib)
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as name:
            cfg = Path(name) / "config.yaml"
            cfg.write_bytes(off)
            guest = Guest.__new__(Guest)
            guest.learning_phase = {"config": str(cfg), "pair": {"on": on, "off": off},
                                    "tray": {"owner": ":1.own", "identity": [42, "10"]},
                                    "current": {"identity": {"daemon": [41, "9"]}}}
            guest.m = {"lifecycle": {"tray_menu_path": "/public/menu"}}
            guest.require_owned_learning_phase = Mock()
            guest.lifecycle_tray_identity = Mock()
            def stats():
                flag["full_stats"] += 1
                if fault == "stats-slow":
                    flag["clock"] += .65
                return {"stats": {"daemon_epoch": 7, "config_generation": flag["generation"],
                                  "config_pending": 0, "config_result": "ok"}}
            guest.lifecycle_stats = stats
            def public(pid, *, _deadline):
                self.assertEqual(pid, 41)
                self.assertGreater(_deadline, flag["clock"])
                flag["public_stats"].append((pid, _deadline))
                if fault in ("stats-peer", "stats-deadline"):
                    raise Failure("STATS peer differs" if fault == "stats-peer" else "STATS shared deadline expired")
                epoch = 8 if fault == "stats-epoch" or fault == "stats-commit-epoch" and "Save" in flag["calls"] else 7
                return {"daemon_epoch": epoch, "config_generation": flag["generation"],
                        "config_pending": 0, "config_result": "ok"}
            def toggle():
                flag["enabled"] = not flag["enabled"]
                flag["calls"].append("toggle")
                return True
            def commit():
                flag["calls"].append("Save")
                cfg.write_bytes(on if fault != "hash" else b"unrelated-config-change")
                flag["generation"] = 0 if fault == "generation" else 1
                flag["closed"] = fault != "dialog"
                return True
            for current, action in ((checkbox, toggle), (save, commit)):
                interface = Mock()
                interface.get_n_actions.return_value = 1
                interface.do_action.side_effect = lambda _index, action=action: action()
                current.get_action_iface.return_value = interface
            def visible(argv, **_kwargs):
                flag["searches"].append(argv)
                matched = "--all" in argv and "--onlyvisible" in argv
                absent = matched and (flag["closed"] or fault == "map-absent" or
                                      fault == "map-race" and len(flag["searches"]) == 1)
                code, output, errors = (1, b"", b"") if absent else (0, b"99\n" if matched else b"99\n100\n101\n", b"")
                if not flag["closed"]:
                    if fault == "map-command":
                        code, output, errors = 2, b"", b"error"
                    elif fault == "map-stderr":
                        code, output, errors = 1, b"", b"error"
                    elif fault == "map-duplicate":
                        output = b"99\n100\n"
                    elif fault == "map-invalid":
                        output = b"invalid\n"
                return SimpleNamespace(returncode=code, stdout=output, stderr=errors)
            def tool(argv, **kwargs):
                if argv[1] == "search":
                    result = visible(argv, **kwargs)
                    if result.returncode:
                        raise Failure("command exit " + str(result.returncode) + ": " + result.stderr.decode())
                    return result.stdout.decode()
                return "43" if fault == "map-foreign" else "42" if argv[1] == "getwindowpid" else "99"
            result = error = None
            with patch.dict(sys.modules, {"gi": gi, "gi.repository": repository}), \
                    patch("installed_guest.checked_run", side_effect=tool), \
                    patch("installed_guest.public_stats", side_effect=public), \
                    patch("installed_guest.subprocess.run", side_effect=visible), \
                    patch("installed_guest.time.monotonic", side_effect=clock), \
                    patch("installed_guest.time.sleep"):
                try:
                    result = guest.lifecycle_sound_save(True)
                except Failure as caught:
                    error = caught
            return result, error, flag, cfg.read_bytes()

    def test_lifecycle_real_sound_save_producer_dirty_cycle_and_commit(self):
        result, error, flags, raw = self.sound_recipe()
        self.assertIsNone(error)
        self.assertTrue(result["sound"])
        self.assertEqual(result["stats"]["config_generation"], 1)
        self.assertEqual(flags["calls"], ["GetLayout", "Event", "toggle", "toggle", "toggle", "Save"])
        self.assertEqual(raw, b"sound:\n  enabled: true")
        expected = ["xdotool", "search", "--all", "--onlyvisible", "--pid", "42", "--name", "^Звук исправлений Punto Switcher$"]
        self.assertEqual(flags["searches"], [expected, expected])

    def test_sound_save_polls_only_pinned_public_stats_under_original_deadline(self):
        result, error, flags, raw = self.sound_recipe("stats-slow")
        self.assertIsNone(error)
        self.assertTrue(result["sound"])
        self.assertEqual(flags["full_stats"], 1)
        self.assertEqual(flags["calls"], ["GetLayout", "Event", "toggle", "toggle", "toggle", "Save"])
        self.assertEqual(len(flags["public_stats"]), 4)
        self.assertEqual(len(set(flags["public_stats"])), 1)
        self.assertLess(flags["clock"], 3.65)
        self.assertEqual(raw, b"sound:\n  enabled: true")
        for fault in ("stats-peer", "stats-epoch", "stats-commit-epoch", "stats-deadline"):
            with self.subTest(fault=fault):
                result, error, flags, raw = self.sound_recipe(fault)
                self.assertIsNone(result)
                self.assertIsInstance(error, Failure)
                self.assertEqual(flags["full_stats"], 1)
                self.assertEqual(flags["calls"].count("Save"), int(fault == "stats-commit-epoch"))

    def test_sound_cleanup_closes_only_conjunctive_owned_visible_dialog(self):
        from types import SimpleNamespace
        for mode in ("owned", "ambiguous", "foreign-pid", "absent", "changed-tray"):
            with self.subTest(mode=mode):
                guest = Guest.__new__(Guest)
                guest.learning_phase = {"dialog_pending": True, "tray": {"identity": [42, "start"]}}
                guest.lifecycle_tray_identity = Mock(side_effect=Failure("changed tray") if mode == "changed-tray" else None)
                closed = []
                def search(argv, **_kwargs):
                    self.assertEqual(argv, ["xdotool", "search", "--all", "--onlyvisible", "--pid", "42", "--name",
                        "^Звук исправлений Punto Switcher$"])
                    # Same class: own Sound, unrelated same-PID title, foreign-PID Sound.
                    output = b"" if mode == "absent" or closed else b"99\n100\n" if mode == "ambiguous" else b"99\n"
                    return SimpleNamespace(returncode=0 if output else 1, stdout=output)
                def tool(argv, **_kwargs):
                    if argv[1] == "getwindowpid":
                        return "43" if mode == "foreign-pid" else "42"
                    self.assertEqual(argv, ["xdotool", "windowclose", "99"])
                    closed.append("99")
                    return ""
                with patch("installed_guest.subprocess.run", side_effect=search) as query, \
                        patch("installed_guest.checked_run", side_effect=tool), patch("installed_guest.time.sleep"):
                    if mode in ("owned", "absent"):
                        guest.lifecycle_sound_close()
                        self.assertFalse(guest.learning_phase["dialog_pending"])
                        self.assertEqual(closed, ["99"] if mode == "owned" else [])
                    else:
                        with self.assertRaises(Failure):
                            guest.lifecycle_sound_close()
                        self.assertEqual(closed, [])
                        if mode == "changed-tray":
                            query.assert_not_called()

    def test_sound_save_waits_only_for_unpublished_window_within_original_deadline(self):
        result, error, flags, raw = self.sound_recipe("map-race")
        self.assertIsNone(error)
        self.assertTrue(result["sound"])
        self.assertEqual(flags["calls"], ["GetLayout", "Event", "toggle", "toggle", "toggle", "Save"])
        self.assertEqual(len(flags["searches"]), 3)
        self.assertLess(flags["clock"], 3)
        self.assertEqual(raw, b"sound:\n  enabled: true")
        for fault in ("map-absent", "map-command", "map-stderr", "map-duplicate", "map-invalid", "map-foreign"):
            with self.subTest(fault=fault):
                result, error, flags, raw = self.sound_recipe(fault)
                self.assertIsNone(result)
                self.assertIsInstance(error, Failure)
                self.assertEqual(flags["calls"], ["GetLayout", "Event"])
                self.assertEqual(raw, b"sound:\n  enabled: false")
                if fault != "map-absent":
                    self.assertEqual(len(flags["searches"]), 1)
                else:
                    self.assertLess(flags["clock"], 3.03)

    def test_lifecycle_real_sound_save_rejects_foreign_pid_and_dirty_sensitivity(self):
        for fault in ("pid", "initial-sensitive", "dirty-insensitive"):
            with self.subTest(fault=fault):
                result, error, flags, raw = self.sound_recipe(fault)
                self.assertIsNone(result)
                self.assertIsInstance(error, Failure)
                self.assertNotIn("Save", flags["calls"])
                self.assertEqual(raw, b"sound:\n  enabled: false")

    def test_lifecycle_real_sound_save_rejects_hash_generation_and_open_dialog(self):
        for fault in ("hash", "generation", "dialog"):
            with self.subTest(fault=fault):
                result, error, flags, raw = self.sound_recipe(fault)
                self.assertIsNone(result)
                self.assertIsInstance(error, Failure)
                self.assertEqual(flags["calls"].count("Save"), 1)


    @staticmethod
    def state(text, group):
        return {"state": {"text": text, "selection": [6, 6], "following_text": "other"},
                "group": group, "clipboard": "sentinel", "pressed": [],
                "identity": {"daemon": [42, "123"]}, "foreground": True}

    def test_desktop_public_reply_checks_six_fields_without_lease_authority(self):
        helper, extension = "1" * 32, "2" * 32
        descriptor = {"pid": 42, "start_time": 123, "helper_instance": helper}
        valid = [0, helper, extension, 7, 8, 1]
        try:
            observed = desktop_receipt(valid, descriptor, [42, "123"], 100)
        except Exception as error:
            observed = error
        self.assertIsInstance(observed, dict, str(observed))
        self.assertEqual(observed["current_role"], 1)
        self.assertEqual(observed["source_generation"], 7)
        self.assertEqual(observed["state_generation"], 8)
        self.assertEqual(observed["observation_signature"], "(usstty)")
        self.assertFalse(any("lease" in key for key in observed))
        for index, bad in ((0, 17), (0, False), (1, "4" * 32), (2, ""), (3, -1),
                           (3, 2**64), (4, True), (5, 255)):
            changed = valid.copy()
            changed[index] = bad
            with self.subTest(index=index, bad=bad), self.assertRaises(Failure):
                desktop_receipt(changed, descriptor, [42, "123"], 100)
        with self.assertRaises(Failure):
            desktop_receipt(valid + [0], descriptor, [42, "123"], 100)
        with self.assertRaises(Failure):
            desktop_receipt(valid, descriptor, [43, "123"], 100)

    def test_desktop_public_receipt_rejects_obsolete_private_lease_tuple(self):
        descriptor = {"pid": 42, "start_time": 123, "helper_instance": "1" * 32}
        with self.assertRaises(Failure):
            desktop_receipt([0, "1" * 32, "2" * 32, "3" * 32, 200, 7, 8, 1],
                            descriptor, [42, "123"], 100)

    def desktop_paste_states(self):
        from copy import deepcopy
        before, final = self.tab_states()
        before["desktop"] = {"outcome": 0, "helper_instance": "1" * 32, "extension_instance": "2" * 32,
            "session_bus_owner": ":1.42", "deadline_us": 200, "observed_us": 100,
            "current_role": 0, "source_generation": 7, "state_generation": 8, "observation_signature": "(usstty)"}
        before["xkb"] = {"group": 0, "locked_group": 0, "base_group": 0, "latched_group": 0,
            "base_mods": 0, "latched_mods": 0, "locked_mods": 0, "ptr_buttons": 0}
        final["desktop"], final["xkb"] = deepcopy(before["desktop"]), deepcopy(before["xkb"])
        final["desktop"].update(current_role=1, state_generation=9)
        final["xkb"].update(group=1, locked_group=1)
        mixed = deepcopy(final)
        mixed["group"] = 0
        mixed["xkb"].update(group=0, locked_group=0)
        return before, mixed, final

    def test_desktop_mixed_paste_snapshot_waits_until_strict_parity(self):
        from copy import deepcopy
        before, mixed, final = self.desktop_paste_states()
        reverse = deepcopy(final)
        reverse["desktop"].update(current_role=0, state_generation=8)
        for transitional in (mixed, reverse):
            collector = Collector({"desktop_parity": True}, Mock(), None, None)
            collector.baseline = deepcopy(before)
            collector.rpc = Mock(side_effect=[transitional, final])
            with patch("installed_guest.time.monotonic", return_value=0), patch("installed_guest.time.sleep"):
                actual = collector.wait_state("gtk", "entry", before, "привет", [6, 6], 1,
                    expected_focus="gtk_text_view", clipboard_payload_restore=True)
            self.assertIs(actual, final)
            self.assertEqual(collector.baseline, before)
            self.assertEqual([call.kwargs["deadline_ns"] for call in collector.rpc.call_args_list], [5_000_000_000] * 2)
            collector.guard.send.assert_not_called()
            with self.assertRaises(Failure):
                verify_desktop(before, transitional, 1)

    def test_desktop_persistent_mixed_snapshot_expires_without_adoption(self):
        from copy import deepcopy
        from itertools import count
        before, mixed, _final = self.desktop_paste_states()
        collector = Collector({"desktop_parity": True}, Mock(), None, None)
        collector.baseline = deepcopy(before)
        collector.rpc = Mock(return_value=mixed)
        ticks = count()
        with patch("installed_guest.time.monotonic", side_effect=lambda: next(ticks) / 4), \
                patch("installed_guest.time.sleep"), self.assertRaises(Failure):
            collector.wait_state("gtk", "entry", before, "привет", [6, 6], 1,
                expected_focus="gtk_text_view", clipboard_payload_restore=True)
        self.assertGreaterEqual(collector.rpc.call_count, 2)
        self.assertGreaterEqual(next(ticks) / 4, 5)
        self.assertEqual(collector.baseline, before)
        collector.guard.send.assert_not_called()

    def test_desktop_transition_rejects_foreign_authority_and_unrequested_roles(self):
        from copy import deepcopy
        before, mixed, _final = self.desktop_paste_states()
        for section, field, value in (("desktop", "helper_instance", "4" * 32),
                ("desktop", "source_generation", 8), ("desktop", "current_role", 2),
                ("desktop", "deadline_us", 100), ("xkb", "group", 2),
                ("xkb", "locked_group", 2), ("xkb", "base_mods", 1)):
            invalid = deepcopy(mixed)
            invalid[section][field] = value
            collector = Collector({"desktop_parity": True}, Mock(), None, None)
            collector.baseline = deepcopy(before)
            collector.rpc = Mock(side_effect=[invalid, AssertionError("foreign authority cannot wait")])
            with self.subTest(field=field), patch("installed_guest.time.monotonic", return_value=0), self.assertRaises(Failure):
                collector.wait_state("gtk", "entry", before, "привет", [6, 6], 1,
                    expected_focus="gtk_text_view", clipboard_payload_restore=True)
            self.assertEqual(collector.rpc.call_count, 1)
            self.assertEqual(collector.baseline, before)
            collector.guard.send.assert_not_called()

    def test_desktop_transition_rejects_generation_regression_between_rpc_samples(self):
        from copy import deepcopy
        before, mixed, _final = self.desktop_paste_states()
        mixed["desktop"]["state_generation"] = 10
        stale = deepcopy(mixed)
        stale["desktop"]["state_generation"] = 9
        collector = Collector({"desktop_parity": True}, Mock(), None, None)
        collector.baseline = deepcopy(before)
        collector.rpc = Mock(side_effect=[mixed, stale, AssertionError("regression cannot wait")])
        with patch("installed_guest.time.monotonic", return_value=0), patch("installed_guest.time.sleep"), self.assertRaises(Failure):
            collector.wait_state("gtk", "entry", before, "привет", [6, 6], 1,
                expected_focus="gtk_text_view", clipboard_payload_restore=True)
        self.assertEqual(collector.rpc.call_count, 2)
        self.assertEqual(collector.baseline, before)
        collector.guard.send.assert_not_called()

    def test_desktop_xkb_state_uses_actual_ctypes_pointer_abi(self):
        import ast
        from types import SimpleNamespace
        from installed_guest import XkbState
        tree = ast.parse((Path(__file__).parent / "installed_guest.py").read_text())
        system = next(node for node in ast.walk(tree) if isinstance(node, ast.FunctionDef) and node.name == "system_observation")
        setup = next(node for node in system.body if isinstance(node, ast.Assign) and
                     isinstance(node.targets[0], ast.Tuple) and any(isinstance(part, ast.Name) and
                     part.id == "xkb" for part in node.targets[0].elts))
        check = next(node for node in system.body if isinstance(node, ast.If) and
                     any(isinstance(part, ast.Attribute) and part.attr == "XkbGetState" for part in ast.walk(node.test)))
        expected = XkbState(group=1, locked_group=1)
        def fill(_display, _device, pointer):
            C.memmove(pointer, C.byref(expected), C.sizeof(expected))
            return 0
        native = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint, C.c_void_p)(fill)
        namespace = {"C": C, "XkbState": XkbState, "Failure": Failure,
                     "self": SimpleNamespace(display=None, x=SimpleNamespace(XQueryKeymap=Mock(return_value=1), XkbGetState=native))}
        exec(compile(ast.Module(body=[setup, check], type_ignores=[]), "XkbGetState", "exec"), namespace)
        self.assertEqual((namespace["xkb"].group, namespace["xkb"].locked_group), (1, 1))

    def test_desktop_parity_rejects_gnome_only_xkb_only_and_changed_instances(self):
        from copy import deepcopy
        receipt = {"outcome": 0, "helper_instance": "1" * 32, "extension_instance": "2" * 32,
                   "session_bus_owner": ":1.42", "deadline_us": 200, "observed_us": 100,
                   "current_role": 0, "source_generation": 7, "state_generation": 8, "observation_signature": "(usstty)"}
        before = {"desktop": receipt, "xkb": {"group": 0, "locked_group": 0, "base_group": 0,
                  "latched_group": 0, "base_mods": 0, "latched_mods": 0, "locked_mods": 2,
                  "ptr_buttons": 0}}
        after = deepcopy(before)
        after["desktop"]["current_role"] = 1
        after["desktop"]["state_generation"] = 9
        after["xkb"].update(group=1, locked_group=1)
        verify_desktop(before, after, 1)
        for section, field, value in (("desktop", "current_role", 0), ("xkb", "group", 0),
                ("xkb", "locked_group", 0), ("xkb", "latched_mods", 1),
                ("xkb", "locked_mods", 0), ("desktop", "helper_instance", "4" * 32),
                ("desktop", "extension_instance", "5" * 32),
                ("desktop", "session_bus_owner", ":1.43"),
                ("desktop", "observation_signature", "(ussttty)"),
                ("desktop", "deadline_us", 100),
                ("desktop", "source_generation", 8), ("desktop", "state_generation", 7),
                ("desktop", "state_generation", 8)):
            changed = deepcopy(after)
            changed[section][field] = value
            with self.subTest(field=field), self.assertRaises(Failure):
                verify_desktop(before, changed, 1)


    def desktop_public_fixture(self, *, drift=None, late_stage=None, refusal=0,
                               latency_ns=0, credential_fault=None, fault_stage=None, fault_code="cancelled",
                               deadline_ns=None):
        import os
        import socket
        from types import SimpleNamespace
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        runtime = Path(temporary.name)
        directory = runtime / "punto"
        directory.mkdir(mode=0o700)
        helper = "1" * 32
        path = directory / ("session-bridge-" + helper + ".sock")
        listener = socket.socket(socket.AF_UNIX)
        self.addCleanup(listener.close)
        listener.bind(str(path))
        path.chmod(0o600)
        pid = os.getpid()
        start = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19]
        descriptor = {"protocol_version": 1, "helper_instance": helper, "pid": pid,
                      "start_time": int(start), "socket_path": str(path)}
        descriptor_path = directory / "session-bridge.json"
        descriptor_path.write_text(json.dumps(descriptor))
        descriptor_path.chmod(0o600)
        bus, cancellable, timer = Mock(), Mock(), Mock()
        timer.is_alive.return_value = False
        clock, calls, read_done = [0], [], [False]
        class GioFault(Exception):
            def matches(self, domain, code):
                return domain == "gio" and code == fault_code
        def fault(stage):
            if fault_stage == stage:
                raise GioFault("desktop transport " + fault_code)
        def expire(stage):
            if late_stage == stage:
                clock[0] = 100_000_000
        def acquired(*_args):
            fault("acquisition")
            expire("acquisition")
            return bus
        def called(destination, object_path, interface, method, params, reply_type, flags, timeout, cancel):
            after = read_done[0]
            stage = ("post-" if after else "pre-") + {
                "GetNameOwner": "owner", "GetConnectionUnixProcessID": "pid",
                "GetConnectionUnixUser": "uid", "GetConnectionCredentials": "credentials"}.get(method, "read")
            calls.append((destination, object_path, interface, method, params, flags, timeout, cancel))
            clock[0] += latency_ns
            fault("read" if method == "ReadState" else stage)
            expire("read" if method == "ReadState" else stage)
            if method == "GetNameOwner":
                value = (":1.43" if drift == "owner" and after else ":1.42",)
            elif method == "GetConnectionUnixProcessID":
                value = (pid + 1 if drift == ("post-pid" if after else "pre-pid") else pid,)
            elif method == "GetConnectionUnixUser":
                value = (os.getuid() + 1 if drift == ("post-uid" if after else "pre-uid") else os.getuid(),)
            elif method == "GetConnectionCredentials":
                phase = "post" if after else "pre"
                values = {"ProcessID": ("u", pid + 1 if drift == phase + "-pid" else pid),
                          "UnixUserID": ("u", os.getuid() + 1 if drift == phase + "-uid" else os.getuid()),
                          "LinuxSecurityLabel": ("ay", b"optional")}
                if credential_fault and credential_fault[0] == phase:
                    _, key, signature, scalar = credential_fault
                    if signature is None:
                        del values[key]
                    else:
                        values[key] = (signature, scalar)
                fields = {key: Mock(**{"get_type_string.return_value": signature,
                                      "unpack.return_value": scalar}) for key, (signature, scalar) in values.items()}
                dictionary = Mock(**{"get_type_string.return_value": "a{sv}"})
                dictionary.lookup_value.side_effect = lambda key, _type: fields.get(key)
                return Mock(**{"get_child_value.return_value": dictionary})
            elif method == "ReadState":
                read_done[0] = True
                if drift == "descriptor":
                    descriptor_path.write_text(json.dumps(descriptor) + "\n")
                value = (refusal, helper, "2" * 32, 7, 8, 0)
            else:
                raise AssertionError("unexpected desktop method: " + method)
            return Mock(**{"unpack.return_value": value})
        bus.call_sync.side_effect = called
        gio = Mock()
        gio.io_error_quark.return_value = "gio"
        gio.IOErrorEnum.CANCELLED, gio.IOErrorEnum.TIMED_OUT = "cancelled", "timed-out"
        gio.Cancellable.return_value = cancellable
        gio.bus_get_sync.side_effect = acquired
        gio.dbus_is_unique_name.side_effect = lambda owner: owner in (":1.42", ":1.43")
        gio.DBusCallFlags.NO_AUTO_START = 17
        gio.DBusConnection.new_for_address_sync.side_effect = Failure("private channel is not public observation", "ABORTED_SAFETY")
        glib = Mock()
        glib.Error = GioFault
        glib.Variant.side_effect = lambda signature, values: (signature, values)
        timer.cancel.side_effect = lambda: expire("retirement")
        timer.join.side_effect = lambda **_: expire("join")
        original_read = Path.read_bytes
        reads = [0]
        def read_bytes(item):
            value = original_read(item)
            if item == descriptor_path:
                reads[0] += 1
                if reads[0] > 1:
                    expire("validation")
            return value
        guest = Guest.__new__(Guest)
        guest.m = {"run_id": "11111111-1111-1111-1111-111111111111"}
        guest.deadline_ns = deadline_ns
        with patch.dict("installed_guest.os.environ", {"XDG_RUNTIME_DIR": str(runtime)}), \
                patch.dict(sys.modules, {"gi.repository": SimpleNamespace(Gio=gio, GLib=glib)}), \
                patch("installed_guest.threading.Timer", return_value=timer), \
                patch("installed_guest.time.monotonic_ns", side_effect=lambda: clock[0]), \
                patch.object(Path, "read_bytes", read_bytes):
            try:
                result = guest.desktop([pid, start])
            except Exception as error:
                result = error
        return result, bus, gio, cancellable, timer, calls

    def test_desktop_public_uses_pinned_session_owner_without_private_connection(self):
        result, bus, gio, cancellable, timer, calls = self.desktop_public_fixture()
        gio.DBusConnection.new_for_address_sync.assert_not_called()
        self.assertIsInstance(result, dict, str(result))
        self.assertEqual(result["session_bus_owner"], ":1.42")
        self.assertEqual(result["observation_signature"], "(usstty)")
        self.assertLess(result["observed_us"], result["deadline_us"])
        self.assertFalse(any("lease" in key for key in result))
        bus.close_sync.assert_not_called()
        self.assertEqual([call[3] for call in calls], [
            "GetNameOwner", "GetConnectionCredentials", "ReadState", "GetNameOwner", "GetConnectionCredentials"])
        read = calls[2]
        self.assertEqual(read[:4], (":1.42", "/org/punto/Switcher/DesktopObserver1",
                                   "org.punto.Switcher.DesktopObserver1", "ReadState"))
        self.assertEqual(read[4], ("(t)", (100_000,)))
        for call in calls:
            self.assertEqual(call[5], 17)
            self.assertTrue(0 < call[6] <= 100)
            self.assertIs(call[7], cancellable)
        timer.cancel.assert_called_once()
        self.assertIn("timeout", timer.join.call_args.kwargs)

    def test_desktop_public_rejects_owner_credentials_and_descriptor_drift(self):
        for drift in ("owner", "pre-pid", "post-pid", "pre-uid", "post-uid", "descriptor"):
            with self.subTest(drift=drift):
                result, _bus, gio, _cancel, _timer, calls = self.desktop_public_fixture(drift=drift)
                gio.DBusConnection.new_for_address_sync.assert_not_called()
                self.assertIsInstance(result, Failure)
                self.assertEqual(result.kind, "ABORTED_SAFETY")
                self.assertEqual(sum(call[3] == "ReadState" for call in calls), 0 if drift.startswith("pre-") else 1)

    def test_desktop_public_rejects_late_acquisition_rpc_validation_and_retirement(self):
        for stage in ("acquisition", "pre-owner", "pre-credentials", "read", "post-owner",
                      "post-credentials", "validation", "retirement", "join"):
            with self.subTest(stage=stage):
                result, bus, gio, _cancel, timer, _calls = self.desktop_public_fixture(late_stage=stage)
                gio.DBusConnection.new_for_address_sync.assert_not_called()
                self.assertIsInstance(result, Failure)
                self.assertEqual(result.kind, "INVALID_OBSERVATION")
                bus.close_sync.assert_not_called()
                timer.cancel.assert_called_once()

    def test_desktop_credentials_five_round_trips_keep_original_deadline_headroom(self):
        result, _bus, _gio, _cancel, _timer, calls = self.desktop_public_fixture(latency_ns=15_000_000)
        self.assertIsInstance(result, dict, str(result))
        self.assertEqual(len(calls), 5)
        self.assertEqual(result["deadline_us"], 100_000)
        self.assertEqual(result["observed_us"], 75_000)

    def test_desktop_credentials_require_both_exact_uint32_variants_before_and_after(self):
        for phase in ("pre", "post"):
            for key in ("ProcessID", "UnixUserID"):
                for signature, scalar in ((None, None), ("b", True), ("s", "0"), ("t", 0),
                                          ("u", True), ("u", -1), ("u", 2**32), ("u", "0")):
                    with self.subTest(phase=phase, key=key, signature=signature, scalar=scalar):
                        result, _bus, _gio, _cancel, _timer, calls = self.desktop_public_fixture(
                            credential_fault=(phase, key, signature, scalar))
                        self.assertIsInstance(result, Failure)
                        self.assertEqual(result.kind, "ABORTED_SAFETY")
                        self.assertEqual(sum(call[3] == "ReadState" for call in calls), int(phase == "post"))

    def test_desktop_gio_cancel_and_timeout_preserve_cause_and_current_call_context(self):
        for code in ("cancelled", "timed-out"):
            for stage, method in (("acquisition", "session bus acquisition"), ("pre-owner", "GetNameOwner"),
                                  ("pre-credentials", "GetConnectionCredentials"), ("read", "ReadState"),
                                  ("post-owner", "GetNameOwner"), ("post-credentials", "GetConnectionCredentials")):
                with self.subTest(code=code, stage=stage):
                    result, _bus, _gio, _cancel, timer, calls = self.desktop_public_fixture(fault_stage=stage, fault_code=code)
                    self.assertIsInstance(result, Failure)
                    self.assertEqual(result.kind, "INVALID_OBSERVATION")
                    self.assertIsNotNone(result.__cause__)
                    self.assertIn("desktop transport " + code, str(result.__cause__))
                    self.assertTrue(any(method in note for note in result.__notes__))
                    timer.cancel.assert_called_once()
                    self.assertLessEqual(len(calls), 5)
        result, _bus, _gio, _cancel, _timer, _calls = self.desktop_public_fixture(fault_stage="read", fault_code="failed")
        self.assertNotIsInstance(result, Failure)
        self.assertEqual(str(result), "desktop transport failed")

    def test_desktop_credentials_keep_shorter_caller_deadline(self):
        result, _bus, _gio, _cancel, _timer, calls = self.desktop_public_fixture(deadline_ns=50_000_000)
        self.assertIsInstance(result, dict, str(result))
        self.assertEqual(result["deadline_us"], 50_000)
        self.assertTrue(all(0 < call[6] <= 50 for call in calls))

    def test_desktop_public_refusal_has_no_private_fallback_and_next_read_recovers(self):
        result, _bus, gio, _cancel, _timer, _calls = self.desktop_public_fixture(refusal=10)
        gio.DBusConnection.new_for_address_sync.assert_not_called()
        self.assertIsInstance(result, Failure)
        fresh, _bus, gio, _cancel, _timer, _calls = self.desktop_public_fixture()
        gio.DBusConnection.new_for_address_sync.assert_not_called()
        self.assertIsInstance(fresh, dict, str(fresh))

    def test_desktop_sequence_checks_each_role_transition_not_only_run_baseline(self):
        from copy import deepcopy
        from installed_guest import verify_desktop
        desktop = {"outcome": 0, "helper_instance": "1" * 32, "extension_instance": "2" * 32,
                   "session_bus_owner": ":1.42", "deadline_us": 200, "observed_us": 100,
                   "current_role": 0, "source_generation": 7, "state_generation": 8,
                   "observation_signature": "(usstty)"}
        base = {**self.state("", 0), "desktop": desktop,
                "xkb": {"group": 0, "locked_group": 0, "base_group": 0, "latched_group": 0,
                        "base_mods": 0, "latched_mods": 0, "locked_mods": 0, "ptr_buttons": 0}}
        collector = Collector({"desktop_parity": True}, Mock(), None, None)
        collector.authority(base)
        changed = deepcopy(base)
        changed["group"] = changed["desktop"]["current_role"] = 1
        changed["desktop"]["state_generation"] = 10
        changed["xkb"].update(group=1, locked_group=1)
        collector.authority(changed)
        stale = deepcopy(changed)
        stale["desktop"]["state_generation"] = 9
        # This stale value is above the run baseline, so only the immediate
        # prior observation reveals the actual regression.
        verify_desktop(base, stale, 1)
        with self.assertRaises(Failure):
            collector.authority(stale)
        missing_advance = deepcopy(changed)
        missing_advance["group"] = missing_advance["desktop"]["current_role"] = 0
        missing_advance["xkb"].update(group=0, locked_group=0)
        with self.assertRaises(Failure):
            collector.authority(missing_advance)
        self.assertIs(collector.baseline, base)


    @staticmethod
    def terminal_admission():
        return {"surface": "terminal", "run_id": "run", "nonce": "abc123", "xid": 42,
                "server": [100, "10"], "reader": [102, "12"], "monitor": [101, "11"],
                "expires_ns": 100}

    def test_terminal_enter_is_not_a_general_key_admission(self):
        from installed_guest import terminal_enter_payload
        with self.assertRaises(Failure):
            qmp_keys(["enter"])
        for changed in ({"surface": "gtk"}, {"xid": 0}, {"nonce": ""},
                        {"server": []}, {"reader": []}, {"expires_ns": 49}):
            admission = {**self.terminal_admission(), **changed}
            run = Mock()
            with patch("installed_guest.time.monotonic_ns", return_value=50), self.assertRaises(Failure):
                Guard("punto-r23-native", "uuid", run).send(["enter"], terminal_admission=admission)
            run.assert_not_called()
        with patch("installed_guest.time.monotonic_ns", return_value=50):
            events = json.loads(terminal_enter_payload(self.terminal_admission()))["arguments"]["events"]
        self.assertEqual([(e["data"]["key"]["data"], e["data"]["down"]) for e in events],
                         [("ret", True), ("ret", False)])

    def test_terminal_enter_expiry_during_domain_check_cannot_send_qmp(self):
        run = Mock(side_effect=["uuid", "running"])
        with patch("installed_guest.time.monotonic_ns", side_effect=[50, 101]), self.assertRaises(Failure):
            Guard("punto-r23-native", "uuid", run).send(["enter"], terminal_admission=self.terminal_admission())
        self.assertEqual(run.call_count, 2)

    def test_canonical_reader_rejects_orphan_group_wrong_nonce_and_control_policy(self):
        import termios
        from installed_guest import terminal_ready
        directory = Path("/private/terminal-abc123")
        ready = {"nonce": "abc123", "directory": str(directory), "pid": 102, "pgrp": 102,
                 "foreground_pgrp": 102, "monitor": 101, "monitor_pgrp": 101, "sid": 100, "monitor_sid": 100,
                 "lflag": termios.ICANON | termios.ISIG | termios.ECHO,
                 "vsusp_hex": "1a", "tty": "/dev/pts/7"}
        self.assertEqual(terminal_ready(ready, "abc123", directory), ready)
        for changed in ({"nonce": "foreign"}, {"directory": "/tmp"},
                        {"foreground_pgrp": 101}, {"monitor_pgrp": 102}, {"monitor_sid": 101},
                        {"lflag": termios.ICANON | termios.ECHO}, {"vsusp_hex": "00"}, {"tty": "/dev/tty"}):
            with self.subTest(changed=changed), self.assertRaises(Failure):
                terminal_ready({**ready, **changed}, "abc123", directory)

    def test_terminal_private_id_collision_precedes_process_or_foreground_mutation(self):
        guest = Guest.__new__(Guest)
        guest.terminal = None
        guest.m = {"terminal_phase": True, "terminal": {"client": "/client", "client_sha256": "a",
                   "server": "/server", "server_sha256": "b", "python_sha256": "c"}, "python": "/python"}
        guest.clipboard_reader = Mock(spec=Lines)
        guest.terminal_bus = Mock(return_value=True)
        guest.system = Mock()
        with patch("installed_guest.check_files"), patch("installed_guest.subprocess.Popen") as spawn, self.assertRaises(Failure):
            guest.terminal_start("abc123")
        spawn.assert_not_called()
        guest.system.assert_not_called()

    def test_terminal_requires_independent_clipboard_and_owned_learning_boundaries(self):
        guest = Guest.__new__(Guest)
        guest.terminal = None
        guest.m = {"terminal_phase": True}
        guest.independent_clipboard_snapshot = Mock()
        with patch("installed_guest.subprocess.Popen") as spawn, self.assertRaises(Failure):
            guest.terminal_start("abc123")
        spawn.assert_not_called()
        guest.independent_clipboard_snapshot.assert_not_called()
        guest.clipboard_reader = Mock(spec=Lines)
        guest.m.update(terminal={"client": "/client", "client_sha256": "a",
                                 "server": "/server", "server_sha256": "b", "python_sha256": "c"}, python="/python")
        with patch("installed_guest.check_files"), patch("installed_guest.subprocess.Popen") as spawn, self.assertRaises(Failure):
            guest.terminal_start("abc123", automatic=True)
        spawn.assert_not_called()

    def test_terminal_paste_routes_restore_before_every_following_input(self):
        import base64
        from copy import deepcopy
        for mode in ("word", "repeat", "auto-undo"):
            for direction in (0, 1):
                with self.subTest(mode=mode, direction=direction), tempfile.TemporaryDirectory() as directory:
                    before = self.state("", direction)
                    before["state"].update(selection=[0, 0], following_text="prompt")
                    fixture, _, _ = self.lifecycle_fixture_snapshots()
                    before["clipboard"] = fixture["clipboard"]
                    before["identity"]["gtk_observer"] = [222, "gtk-start"]
                    collector = Collector({"lifecycle_phase": True, "sources": {}, "package": {"path": "/package", "sha256": "hash"}},
                                          Mock(), Mock(), Path(directory))
                    collector.lifecycle_fixture_started = True
                    collector.baseline = deepcopy(before)
                    admission = {"nonce": "nonce"}
                    state = {"publication": False, "snapshot": None, "entered": False, "owner": before["clipboard"]["owner"], "routes": []}
                    def rpc(_surface, action, **_kwargs):
                        if action == "start":
                            return {**deepcopy(before), "terminal_admission": admission, "terminal_identity": admission}
                        if action == "status":
                            return {"enabled": mode == "auto-undo"}
                        if action == "close":
                            return {**deepcopy(collector.baseline), "terminal_closed": True}
                        if action == "lifecycle-fixture":
                            self.assertTrue(_kwargs["reassert"])
                            self.assertEqual(_kwargs["deadline_ns"], state["snapshot_deadline"])
                            prior = deepcopy(state["snapshot"])
                            following = deepcopy(prior)
                            following["clipboard"].update(owner=22, publication_generation=prior["clipboard"]["publication_generation"] + 1)
                            state["owner"], state["snapshot"] = 22, following
                            return {"before": prior, "after": following, "publication": {"pid": 222, "owner": 22,
                                "timestamp": 123, "publication_generation": 1, "observer_instance_id": "owned-gtk"}}
                        state["snapshot_deadline"] = _kwargs.get("deadline_ns")
                        result = deepcopy(state["snapshot"])
                        result["submitted_base64"] = base64.b64encode((result["state"]["text"] + "\n").encode()).decode() if state["entered"] else None
                        return result
                    collector.rpc = Mock(side_effect=rpc)
                    collector.arm = Mock(return_value=("attempt", deepcopy(before)))
                    collector.type_keys = Mock()
                    def key(_surface, *keys, **_kwargs):
                        self.assertEqual(collector.baseline["clipboard"]["owner"], state["owner"])
                        state["publication"] = "pause" in keys or keys == ("ctrl", "z") or mode == "auto-undo" and keys == ("space",)
                        return 0
                    collector.key = Mock(side_effect=key)
                    def wait(surface, widget, baseline, text, selection, group, **kwargs):
                        observed = deepcopy(baseline)
                        observed.update(group=group, terminal_identity=admission)
                        observed["state"].update(text=text, selection=selection)
                        if state["publication"]:
                            state["owner"] += 1
                            observed["clipboard"]["owner"] = state["owner"]
                            state["routes"].append(kwargs.get("clipboard_payload_restore"))
                            state["publication"] = False
                        state["snapshot"] = observed
                        result = Collector.wait_state(collector, surface, widget, baseline, text, selection, group, **kwargs)
                        if kwargs.get("clipboard_payload_restore"):
                            clock[0] += 0.25
                        return result
                    collector.wait_state = Mock(side_effect=wait)
                    collector.terminal_enter = Mock(side_effect=lambda: state.__setitem__("entered", True))
                    collector.feature_finish = Mock()
                    clock = [0]
                    with patch("installed_guest.uuid.uuid4", return_value=Mock(hex="nonce")), patch("installed_guest.check_files"), \
                            patch("installed_guest.time.monotonic", side_effect=lambda: clock[0]), \
                            patch("installed_guest.time.monotonic_ns", side_effect=lambda: int(clock[0] * 1e9)), \
                            patch("installed_guest.time.sleep", side_effect=lambda _: clock.__setitem__(0, clock[0] + 5)):
                        collector.terminal_case(mode)
                    self.assertEqual(state["routes"], [True] * {"word": 1, "repeat": 5, "auto-undo": 3}[mode])
                    self.assertEqual(collector.baseline["clipboard"]["owner"], state["owner"])
                    collector.terminal_enter.assert_called_once()
                    collector.feature_finish.assert_called_once()
                    records = (Path(directory) / "lifecycle-fixture-reassert.jsonl").read_text().splitlines()
                    self.assertEqual(len(records), len(state["routes"]))

    def test_lifecycle_fixture_reassert_keeps_terminal_authority_and_updates_only_phase_clipboard(self):
        from copy import deepcopy
        for fault in (None, "unarmed", "not-started", "authorization", "payload", "process", "observer", "held", "publication-owner", "foreground"):
            with self.subTest(fault=fault):
                before, after, proof = self.lifecycle_fixture_snapshots()
                before["terminal_identity"] = after["terminal_identity"] = {"nonce": "nonce", "server": [99, "start"]}
                phase_current = deepcopy(before)
                phase_current["clipboard"]["owner"] = 9
                if fault == "payload":
                    before["clipboard"]["representations"]["TEXT"][2] = "Zm9yZWlnbg=="
                if fault == "process":
                    before["identity"]["helper"] = [999, "foreign"]
                if fault == "held":
                    before["pressed"] = ["Control_L"]
                if fault == "publication-owner":
                    proof["owner"] = 99
                if fault == "foreground":
                    after["foreground"] = False
                guest = Guest.__new__(Guest)
                guest.learning_phase = {"armed": fault != "unarmed", "current": deepcopy(phase_current)}
                guest.lifecycle_fixture_started = fault != "not-started"
                guest.lifecycle_fixture_observer = "owned-gtk"
                guest.surface = "terminal"
                guest.deadline_ns = None
                guest.system = Mock(side_effect=[before, after])
                guest.target = Mock(side_effect=AssertionError("terminal target must not receive GTK fixture request"))
                guest.gtk = Mock()
                guest.gtk.call.side_effect = [{"observer_instance_id": "foreign" if fault == "observer" else "owned-gtk"}, {"publication": proof}]
                if fault is None:
                    result = guest.lifecycle_fixture(deepcopy(before["clipboard"]), reassert=True)
                    self.assertEqual(result["after"]["terminal_identity"], before["terminal_identity"])
                    self.assertEqual(guest.surface, "terminal")
                    self.assertEqual(guest.learning_phase["current"], {**phase_current, "clipboard": after["clipboard"]})
                    self.assertTrue(guest.gtk.call.call_args.args[0]["reassert"])
                else:
                    with self.assertRaises(Failure):
                        guest.lifecycle_fixture(deepcopy(before["clipboard"]), reassert="yes" if fault == "authorization" else True)
                    self.assertEqual(guest.learning_phase["current"], phase_current)
                guest.target.assert_not_called()

    def test_terminal_paste_rejects_changed_representations_and_foreign_authority(self):
        from copy import deepcopy
        for fault in ("type", "format", "bytes", "identity", "focus", "following"):
            before, observed = self.tab_states()
            before["state"]["focus_target"] = observed["state"]["focus_target"] = "terminal"
            if fault in ("type", "format", "bytes"):
                observed["clipboard"]["representations"]["UTF8_STRING"][{"type": 0, "format": 1, "bytes": 2}[fault]] = {
                    "type": "STRING", "format": 16, "bytes": "Yg=="}[fault]
            elif fault == "identity":
                observed["identity"]["daemon"] = [43, "foreign"]
            elif fault == "focus":
                observed["state"]["focus_target"] = "foreign"
            else:
                observed["state"]["following_text"] = "foreign"
            collector = Collector({}, Mock(), None, None)
            collector.baseline = deepcopy(before)
            collector.rpc = Mock(return_value=observed)
            clock = [0]
            with self.subTest(fault=fault), patch("installed_guest.time.monotonic", side_effect=lambda: clock[0]), \
                    patch("installed_guest.time.sleep", side_effect=lambda _: clock.__setitem__(0, 5)), self.assertRaises(Failure):
                collector.wait_state("terminal", "terminal", before, "привет", [6, 6], 1, clipboard_payload_restore=True)
            self.assertEqual(collector.baseline, before)
            collector.guard.send.assert_not_called()

    def test_terminal_identity_admission_does_not_replace_core_or_clipboard_baseline(self):
        baseline = {"identity": {"daemon": [42, "1"]}, "clipboard": {"owner": 9},
                    "pressed": [], "foreground": True, "group": 0}
        collector = Collector({}, Mock(), Mock(), None)
        collector.authority(baseline)
        identity = {"server": [100, "10"], "reader": [102, "12"], "nonce": "abc123", "xid": 50}
        collector.terminal_admission = identity
        observed = {**baseline, "terminal_identity": identity}
        collector.authority(observed, foreground=True)
        self.assertIs(collector.baseline, baseline)
        for changed in ({"terminal_identity": {**identity, "xid": 51}},
                        {"identity": {"daemon": [42, "2"]}}, {"clipboard": {"owner": 10}},
                        {"foreground": False}, {"pressed": ["ctrl"]}):
            with self.subTest(changed=changed), self.assertRaises(Failure):
                collector.authority({**observed, **changed}, foreground=True)

    def test_terminal_cleanup_signals_captured_pidfds_not_reused_pids(self):
        guest = Guest.__new__(Guest)
        guest.terminal = {"fds": {"server": 31, "reader": 32, "monitor": 33}}
        with patch("installed_guest.signal.pidfd_send_signal") as send, patch("installed_guest.os.close") as close, \
                patch("installed_guest.os.kill") as unsafe:
            guest.terminal_close(force=True)
        self.assertEqual([call.args[0] for call in send.call_args_list], [31, 32, 33])
        self.assertEqual([call.args[0] for call in close.call_args_list], [31, 32, 33])
        unsafe.assert_not_called()
        self.assertIsNone(guest.terminal)


    def test_terminal_pid_reuse_during_pin_never_enters_cleanup_ownership(self):
        guest = Guest.__new__(Guest)
        guest.terminal = {"identity": {}, "fds": {}}
        guest.terminal_process = Mock(side_effect=[[100, "10"], [100, "11"]])
        with patch("installed_guest.os.pidfd_open", return_value=31), patch("installed_guest.os.close") as close, self.assertRaises(Failure):
            guest.terminal_pin("reader", 100, "/python", [100, "10"])
        self.assertEqual(guest.terminal["fds"], {})
        close.assert_called_once_with(31)


    def test_terminal_submission_requires_exact_bytes_not_echo_or_prefix(self):
        import base64
        from installed_guest import terminal_submission
        expected = "приветф\n".encode()
        self.assertFalse(terminal_submission(None, expected))
        self.assertTrue(terminal_submission(base64.b64encode(expected).decode(), expected))
        for value in (expected[:-1], expected + b"x", b"ghbdtna\n", b"echo never-execute\n", b"\x1a\n"):
            with self.subTest(value=value), self.assertRaises(Failure) as error:
                terminal_submission(base64.b64encode(value).decode(), expected)
            self.assertEqual(error.exception.kind, "FAIL_PRODUCT")


    def test_terminal_failure_cannot_skip_any_other_owned_guest_cleanup(self):
        for failing in ("terminal", "gtk", "chrome", "playwright", "clipboard_reader"):
            guest = Guest.__new__(Guest)
            guest.terminal_close, guest.gtk, guest.chrome, guest.playwright = Mock(), Mock(), Mock(), Mock()
            guest.learning_phase = None
            guest.clipboard_reader = Mock(spec=Lines)
            guest.chrome.poll.return_value = None
            callback = {"terminal": guest.terminal_close, "gtk": guest.gtk.close,
                        "chrome": guest.chrome.wait, "playwright": guest.playwright.stop,
                        "clipboard_reader": guest.clipboard_reader.close}[failing]
            callback.side_effect = Failure(failing + " cleanup", "ABORTED_SAFETY")
            with self.subTest(failing=failing), self.assertRaises(Failure) as error:
                guest.close()
            self.assertEqual(error.exception.kind, "ABORTED_SAFETY")
            guest.gtk.close.assert_called_once()
            guest.clipboard_reader.close.assert_called_once()
            guest.chrome.terminate.assert_called_once()
            guest.chrome.wait.assert_called_once_with(timeout=3)
            guest.playwright.stop.assert_called_once()

    def test_terminal_watchdog_runs_each_kill_callback_before_own_group_kill(self):
        import ast
        import contextlib
        import signal
        from types import SimpleNamespace
        tree = ast.parse((Path(__file__).parent / "installed_guest.py").read_text())
        expired = next(node for node in ast.walk(tree) if isinstance(node, ast.FunctionDef) and node.name == "expired")
        guest = SimpleNamespace(terminal_close=Mock(side_effect=Failure("terminal cleanup", "ABORTED_SAFETY")),
                                gtk=SimpleNamespace(child=Mock(pid=21)), chrome=Mock(pid=22),
                                clipboard_reader=SimpleNamespace(child=Mock(pid=23)), lifecycle_restore=Mock())
        guest.gtk.child.poll.return_value = guest.chrome.poll.return_value = guest.clipboard_reader.child.poll.return_value = None
        namespace = {"guest": guest, "owned_group": 20, "signal": signal, "ExitStack": contextlib.ExitStack,
                     "os": SimpleNamespace(killpg=Mock()), "kill_guest_group": Mock()}
        exec(compile(ast.Module(body=[expired], type_ignores=[]), "expired", "exec"), namespace)
        with self.assertRaises(Failure):
            namespace["expired"](None, None)
        self.assertEqual({call.args[0] for call in namespace["os"].killpg.call_args_list}, {21, 22, 23})
        guest.lifecycle_restore.assert_called_once()
        namespace["kill_guest_group"].assert_called_once_with(20)

    def test_terminal_cleanup_safety_overrides_original_product_failure(self):
        for phase in ("case", "cohort"):
            with self.subTest(phase=phase), tempfile.TemporaryDirectory() as directory:
                original = Failure("original product boundary", "FAIL_PRODUCT")
                safety = Failure("cleanup authority lost", "ABORTED_SAFETY")
                collector = Collector({}, Mock(), Mock(), Path(directory))
                collector.baseline = {"identity": {"daemon": [42, "123"]}}
                if phase == "case":
                    identity = {"nonce": "nonce"}
                    collector.rpc = Mock(side_effect=[{"terminal_admission": identity, "terminal_identity": identity}, safety])
                    collector.authority = Mock(side_effect=original)
                    with patch("installed_guest.uuid.uuid4", return_value=Mock(hex="nonce")), self.assertRaises(Failure) as error:
                        collector.terminal_case("word")
                else:
                    collector.rpc = Mock(side_effect=[{"enabled": False}, safety])
                    collector.terminal_case = Mock(side_effect=original)
                    with self.assertRaises(Failure) as error:
                        collector.terminals()
                self.assertIs(error.exception, safety)
                self.assertIs(error.exception.__cause__, original)

    def test_private_pty_foreground_validation_from_foreign_session(self):
        import errno
        import os
        import pty
        import time
        master, slave = pty.openpty()
        self.addCleanup(os.close, master)
        self.addCleanup(os.close, slave)
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent, prefix="terminal-") as temporary:
            directory = Path(temporary).resolve()
            code = ("import fcntl,termios,sys; fcntl.ioctl(0,termios.TIOCSCTTY,0); "
                    "from installed_guest import terminal_reader_main; terminal_reader_main(sys.argv[1],'nonce')")
            child = subprocess.Popen([sys.executable, "-c", code, str(directory)], stdin=slave,
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, start_new_session=True,
                cwd=Path(__file__).resolve().parent)
            try:
                deadline = time.monotonic() + 2
                while not (directory / "ready.json").exists() and child.poll() is None and time.monotonic() < deadline:
                    time.sleep(.01)
                self.assertTrue((directory / "ready.json").exists(), "private canonical reader did not publish")
                ready = json.loads((directory / "ready.json").read_text())
                raw = Path(f"/proc/{ready['pid']}/stat").read_text().rsplit(")", 1)[1].split()
                self.assertEqual(int(raw[5]), ready["pgrp"])
                self.assertEqual(raw[19], ready["start"])
                self.assertNotEqual(os.getsid(0), ready["sid"])
                fd = os.open(f"/proc/{ready['pid']}/fd/0", os.O_RDONLY | os.O_NOCTTY)
                try:
                    with self.assertRaises(OSError) as foreign:
                        os.tcgetpgrp(fd)
                    self.assertEqual(foreign.exception.errno, errno.ENOTTY)
                finally:
                    os.close(fd)
                guest = Guest.__new__(Guest)
                guest.m = {"python": sys.executable, "terminal": {"client": "/client", "client_sha256": "a",
                    "server": sys.executable, "server_sha256": "b", "python_sha256": "c"}}
                identity = {"nonce": "nonce", "server": guest.terminal_process(os.getpid(), sys.executable),
                    "reader": [ready["pid"], ready["start"]], "monitor": [ready["monitor"], ready["monitor_start"]],
                    "xid": 42, "wm_class": ["gnome-terminal-server", "Gnome-terminal"]}
                guest.terminal = {"identity": identity, "directory": directory, "title": "owned", "app_id": "owned", "owner": "owner"}
                guest.terminal_bus = Mock(side_effect=lambda method, _name: {
                    "GetNameOwner": "owner", "GetConnectionUnixProcessID": os.getpid(), "GetConnectionUnixUser": os.getuid()}[method])
                commands = lambda argv, **_kw: ('WM_CLASS(STRING) = "gnome-terminal-server", "Gnome-terminal"'
                    if argv[0] == "xprop" else str(os.getpid()) if argv[1] == "getwindowpid" else "owned")
                with patch("installed_guest.check_files"), patch("installed_guest.checked_run", side_effect=commands):
                    self.assertEqual(guest.terminal_validate(), identity)
            finally:
                child.terminate()
                child.wait(timeout=2)
                child.stderr.close()

    def test_lifecycle_only_cli_skips_text_cohorts_and_marks_diagnostic_scope(self):
        import installed_guest
        for mode in ("valid", "invalid-status", "pending-clipboard", "held-key", "missing-lifecycle", "legacy", "soak"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                manifest = {"domain_uuid": "owned", "ssh_host": "punto@192.0.2.8", "sources": {"source": "hash"},
                    "installed": {"installed": "hash"}, "processes": {role: role for role in (*PIPELINE_ROLES, "helper", "gnome", "ibus")},
                    "session": {"XDG_SESSION_TYPE": "x11"}, "package": {"path": "/package", "sha256": "hash"},
                    "python": sys.executable, "staged_dir": "/home/punto/punto-acceptance-diagnostic"}
                path = root / "manifest.json"
                path.write_text(json.dumps(manifest))
                for name in ("key", "known_hosts"):
                    (root / name).touch()
                args = ["installed_guest.py", "--vm", "owned", "--uuid", "owned", "--host", manifest["ssh_host"],
                    "--key", str(root / "key"), "--known-hosts", str(root / "known_hosts"),
                    "--expected-payload-manifest", str(path), "--output", str(root / "output"), "--features", "--terminal", "--lifecycle-only"]
                if mode != "missing-lifecycle":
                    args.append("--lifecycle")
                if mode == "legacy":
                    args.extend(["--profile", "exact285-gtk16"])
                if mode == "soak":
                    args.append("--soak")
                authority = self.state("", 0)
                if mode == "pending-clipboard":
                    authority["clipboard"] = {"publication_pending": True}
                if mode == "held-key":
                    authority["pressed"] = ["Shift_L"]
                channel = Mock(**{"read.return_value": {"ready": True}})
                channel.call.side_effect = lambda request, **_kwargs: {
                    **({"enabled": "invalid" if mode == "invalid-status" else False} if request["action"] == "status" else authority),
                    "guest_monotonic_ns": 70, "host_interval_ns": [90, 100]}
                collector = Collector(manifest, Mock(), channel, root / "output")
                collector.operation, collector.features = Mock(), Mock()
                def lifecycle_entry():
                    self.assertIsNotNone(collector.baseline)
                    self.assertEqual(collector.baseline["identity"], authority["identity"])
                    self.assertEqual(collector.baseline["clipboard"], authority["clipboard"])
                    self.assertIs(collector.original_status, False)
                    self.assertEqual(collector.guest_offset_upper_ns, 30)
                    recovery = json.loads((root / "output/status-recovery.json").read_text())
                    self.assertEqual(recovery, {"enabled": False, "daemon": authority["identity"]["daemon"]})
                    self.assertEqual([item.args[0]["action"] for item in channel.call.call_args_list], ["check", "status"])
                    collector.completed, collector.lifecycle_terminal_cases = 24, 8
                collector.lifecycle = Mock(side_effect=lifecycle_entry)
                with patch("installed_guest.sys.argv", args), patch("installed_guest.Guard") as guard, \
                        patch("installed_guest.phase_capabilities"), patch("installed_guest.check_files"), \
                        patch("installed_guest.checked_run", return_value='{"enabled": false}'), \
                        patch("installed_guest.Collector", return_value=collector), patch("installed_guest.Lines", return_value=channel):
                    if mode == "valid":
                        self.assertEqual(installed_guest.main(), 0)
                        summary = json.loads((root / "output/summary.json").read_text())
                        self.assertEqual(summary["execution_mode"], "lifecycle-only")
                        self.assertEqual(summary["scope"], "installed-lifecycle-only-diagnostic")
                        self.assertEqual(summary["skipped_cohorts"], ["base", "features"])
                        self.assertEqual(summary["verified_operations"], 24)
                        self.assertTrue(summary["manifest"]["features"])
                        collector.operation.assert_not_called()
                        collector.features.assert_not_called()
                        collector.lifecycle.assert_called_once()
                    elif mode in ("invalid-status", "pending-clipboard", "held-key"):
                        self.assertEqual(installed_guest.main(), 1)
                        collector.lifecycle.assert_not_called()
                        collector.operation.assert_not_called()
                    else:
                        with self.assertRaises(Failure):
                            installed_guest.main()
                        guard.assert_not_called()

    def test_terminal_changed_live_foreground_or_termios_refuses_enter(self):
        import os
        import pty
        import termios
        from copy import deepcopy
        master, slave = pty.openpty()
        self.addCleanup(os.close, master)
        self.addCleanup(os.close, slave)
        tty = os.ttyname(slave)
        pid, monitor = os.getpid(), os.getppid()
        original_attributes = termios.tcgetattr(slave)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            proc = directory / "proc" / str(pid)
            (proc / "fd").mkdir(parents=True)
            raw = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
            raw[2:6], raw[19] = ["102", "100", str(os.fstat(slave).st_rdev), "102"], "2"
            (proc / "stat").write_text(f"{pid} (fixture) " + " ".join(raw))
            (proc / "fd/0").symlink_to(tty)
            ready = {"nonce": "nonce", "directory": str(directory), "pid": pid, "monitor": monitor,
                     "pgrp": 102, "foreground_pgrp": 102, "monitor_pgrp": 101, "sid": 100, "monitor_sid": 100,
                     "lflag": original_attributes[3], "vsusp_hex": "1a", "tty": tty}
            (directory / "ready.json").write_text(json.dumps(ready))
            identity = {"nonce": "nonce", "server": [99, "1"], "reader": [pid, "2"],
                        "monitor": [monitor, "3"], "xid": 42, "wm_class": ["gnome-terminal-server", "Gnome-terminal"]}
            guest = Guest.__new__(Guest)
            guest.m = {"python": "/python", "terminal": {"client": "/client", "client_sha256": "a",
                       "server": "/server", "server_sha256": "b", "python_sha256": "c"}}
            guest.terminal = {"identity": identity, "directory": directory, "title": "owned",
                              "app_id": "owned", "owner": "owner"}
            guest.terminal_bus = Mock(side_effect=lambda method, _name: {
                "GetNameOwner": "owner", "GetConnectionUnixProcessID": 99, "GetConnectionUnixUser": os.getuid()}[method])
            guest.terminal_process = Mock(side_effect=lambda actual, _exe: next(
                value for name, value in identity.items() if name in ("server", "reader", "monitor") and value[0] == actual))
            collector = Collector({"sources": {}, "package": {"path": "/package", "sha256": "x"}, "run_id": "run"},
                                  Mock(), Mock(), directory)
            collector.guard.identity = "uuid"
            collector.terminal_admission = identity
            collector.check = lambda *_args, **_values: {"terminal_identity": guest.terminal_validate()}
            actual_path = Path
            commands = lambda argv, **_kw: ('WM_CLASS(STRING) = "gnome-terminal-server", "Gnome-terminal"' if argv[0] == "xprop"
                                           else "99" if argv[1] == "getwindowpid" else "owned")
            with patch("installed_guest.check_files"), patch("installed_guest.checked_run", side_effect=commands), \
                    patch("installed_guest.Path", side_effect=lambda value: proc.parent if str(value) == "/proc" else actual_path(value)), \
                    patch("installed_guest.os.getpgid", side_effect=lambda actual: 102 if actual == pid else 101), \
                    patch("installed_guest.os.getsid", return_value=100), \
                    patch("installed_guest.os.tcgetpgrp", side_effect=AssertionError("foreign-session ioctl must not be used")):
                self.assertEqual(guest.terminal_validate(), identity)
                for policy in ("foreground", "pgrp", "session", "tty", "start", "canonical", "signals", "echo", "suspend"):
                    attributes = deepcopy(original_attributes)
                    observed = raw.copy()
                    if policy in ("foreground", "pgrp", "session", "tty", "start"):
                        index = {"foreground": 5, "pgrp": 2, "session": 3, "tty": 4, "start": 19}[policy]
                        observed[index] = str(int(observed[index]) + 1)
                    (proc / "stat").write_text(f"{pid} (fixture) " + " ".join(observed))
                    if policy in ("canonical", "signals", "echo"):
                        attributes[3] &= ~{"canonical": termios.ICANON, "signals": termios.ISIG, "echo": termios.ECHO}[policy]
                    if policy == "suspend":
                        attributes[6][termios.VSUSP] = b"\0"
                    termios.tcsetattr(slave, termios.TCSANOW, attributes)
                    with self.subTest(policy=policy):
                        collector.guard.send.reset_mock()
                        with self.assertRaises(Failure) as error:
                            collector.terminal_enter()
                        self.assertEqual(error.exception.kind, "ABORTED_SAFETY")
                        collector.guard.send.assert_not_called()
                termios.tcsetattr(slave, termios.TCSANOW, original_attributes)

    def terminal_native_fixture(self):
        from types import SimpleNamespace
        text = "PUNTO-nonce\nPUNTO-nonce>"
        class Accessible:
            def get_text(self):
                return self
        native = Accessible()
        native.get_character_count = native.get_caret_offset = Mock(return_value=len(text))
        native.get_n_selections = Mock(return_value=0)
        interface = SimpleNamespace(get_text=Mock(return_value=text),
            get_character_count=Mock(return_value=len(text)), get_caret_offset=Mock(return_value=len(text)),
            get_n_selections=Mock(return_value=0))
        app = Mock()
        app.get_process_id.return_value = 99
        app.get_role.return_value = 1
        app.get_child_count.return_value = 0
        app.get_text_iface.return_value = native
        desktop = Mock()
        desktop.get_child_count.return_value = 1
        desktop.get_child_at_index.return_value = app
        atspi = SimpleNamespace(set_timeout=Mock(), get_desktop=Mock(return_value=desktop),
                                Role=SimpleNamespace(TERMINAL=1), Text=interface)
        guest = Guest.__new__(Guest)
        guest.terminal = {"identity": {"server": [99, "1"], "nonce": "nonce"}}
        return guest, atspi, desktop, app, native

    def test_terminal_text_dispatches_explicit_text_interface_despite_accessible_shadow(self):
        from types import SimpleNamespace
        for mode in ("valid", "wrong-nonce", "selection", "duplicate-app", "duplicate-terminal", "oversized"):
            guest, atspi, desktop, app, native = self.terminal_native_fixture()
            if mode == "wrong-nonce":
                atspi.Text.get_text.return_value = "PUNTO-foreign\nPUNTO-foreign>"
            if mode == "selection":
                atspi.Text.get_n_selections.return_value = 1
            if mode == "duplicate-app":
                desktop.get_child_count.return_value = 2
            if mode == "duplicate-terminal":
                another = Mock(**{"get_role.return_value": 1, "get_child_count.return_value": 0})
                app.get_child_count.return_value = 1
                app.get_child_at_index.return_value = another
            if mode == "oversized":
                atspi.Text.get_character_count.return_value = 4097
            with self.subTest(mode=mode), patch.dict(sys.modules, {
                    "gi": SimpleNamespace(require_version=Mock()), "gi.repository": SimpleNamespace(Atspi=atspi)}):
                if mode == "valid":
                    self.assertEqual(guest.terminal_text(), {"text": "", "selection": [0, 0], "following_text": "PUNTO-nonce"})
                    atspi.Text.get_text.assert_called_once_with(native, 0, len("PUNTO-nonce\nPUNTO-nonce>"))
                    for name in ("get_character_count", "get_caret_offset", "get_n_selections"):
                        getattr(atspi.Text, name).assert_called_once_with(native)
                else:
                    with self.assertRaises(Failure):
                        guest.terminal_text()

    def test_installed_gi_text_interface_call_shapes_are_offline_introspection_contracts(self):
        import gi
        gi.require_version("Atspi", "2.0")
        from gi.repository import Atspi
        self.assertEqual([argument.get_name() for argument in Atspi.Accessible.get_text.get_arguments()], [])
        self.assertEqual([argument.get_name() for argument in Atspi.Text.get_text.get_arguments()],
                         ["start_offset", "end_offset"])
        for name in ("get_character_count", "get_caret_offset", "get_n_selections"):
            self.assertEqual([argument.get_name() for argument in getattr(Atspi.Text, name).get_arguments()], [])

    def test_terminal_native_enumeration_and_subtree_fanout_are_bounded(self):
        from types import SimpleNamespace
        for source in ("desktop", "subtree"):
            guest, atspi, desktop, app, _native = self.terminal_native_fixture()
            oversized = desktop if source == "desktop" else app
            oversized.get_child_count.return_value = 300
            foreign = Mock()
            foreign.get_process_id.return_value = 98
            foreign.get_child_count.return_value = 0
            foreign.get_role.return_value = 0
            oversized.get_child_at_index.side_effect = [app if source == "desktop" else foreign] + [foreign] * 299
            with self.subTest(source=source), \
                    patch.dict(sys.modules, {"gi": SimpleNamespace(require_version=Mock()),
                                            "gi.repository": SimpleNamespace(Atspi=atspi)}), self.assertRaises(Failure):
                guest.terminal_text()
            with self.subTest(source=source, check="no-oversized-allocation"):
                self.assertEqual(oversized.get_child_at_index.call_count, 0)

    def test_terminal_native_deadline_covers_enumeration_fanout_and_text(self):
        from types import SimpleNamespace
        for boundary in ("timeout", "desktop", "count", "child", "pid", "role", "interface", "characters", "text", "caret", "selection"):
            guest, atspi, desktop, app, native = self.terminal_native_fixture()
            clock = [0.0]
            method, value = {"timeout": (atspi.set_timeout, None), "desktop": (atspi.get_desktop, desktop), "count": (desktop.get_child_count, 1),
                             "child": (desktop.get_child_at_index, app), "pid": (app.get_process_id, 99),
                             "role": (app.get_role, 1), "interface": (app.get_text_iface, native),
                             "characters": (atspi.Text.get_character_count, len("PUNTO-nonce\nPUNTO-nonce>")),
                             "text": (atspi.Text.get_text, "PUNTO-nonce\nPUNTO-nonce>"),
                             "caret": (atspi.Text.get_caret_offset, len("PUNTO-nonce\nPUNTO-nonce>")),
                             "selection": (atspi.Text.get_n_selections, 0)}[boundary]
            def delayed(*_args):
                clock[0] = 1.01
                return value
            if boundary == "desktop":
                with patch.dict(sys.modules, {"gi": SimpleNamespace(require_version=Mock()),
                                             "gi.repository": SimpleNamespace(Atspi=atspi)}):
                    self.assertEqual(guest.terminal_text()["selection"], [0, 0])
            method.side_effect = delayed
            with self.subTest(boundary=boundary), patch("installed_guest.time.monotonic", side_effect=lambda: clock[0]), \
                    patch.dict(sys.modules, {"gi": SimpleNamespace(require_version=Mock()),
                                            "gi.repository": SimpleNamespace(Atspi=atspi)}), self.assertRaises(Failure):
                guest.terminal_text()


class LivePackageProviderControls(unittest.TestCase):
    """Source capability gates; real package/runtime qualification is separate."""

    def test_live_provider_has_one_runtime_callback_and_no_recovery_owner(self):
        source = Path(__file__).resolve().parents[2] / "deployment/punto-installed-lifecycle-provider.py"
        self.assertTrue(source.is_file(), "live quiesce/activate/rollback callback capability is absent")
        text = source.read_text()
        for verb in ("quiesce-v1", "rollback-v1"):
            self.assertIn(verb, text)
        self.assertNotIn("dpkg --", text)
        self.assertNotIn("legacy_guard_enter", text)
        self.assertNotIn("snapshot-original", text.split("def main", 1)[-1])

    def test_live_provider_and_collector_share_staged_readonly_facts(self):
        root = Path(__file__).resolve().parents[2]
        facts = root / "tests/recovery/runtime_facts.py"
        self.assertTrue(facts.is_file(), "shared process/pipeline facts capability is absent")
        collector = (root / "tests/recovery/installed_guest.py").read_text()
        self.assertIn("from runtime_facts import", collector)
        self.assertIn('"runtime_facts.py"', collector)
        self.assertNotIn('processes = {}\n        for stat in Path("/proc").glob', collector)

    def test_callback_abi_baseline_fresh_upgrade_and_sealed_rollback_are_explicit(self):
        module, value, _, _ = self.provider()
        value.activate, value.quiesce, value.rollback_profile = Mock(), Mock(), Mock(return_value="original")
        for mode, target in (("baseline285", "exact285"), ("fresh", "candidate"), ("upgrade", "candidate")):
            module.dispatch(value, [], mode)
            value.activate.assert_called_with(target)
            module.dispatch(value, ["quiesce-v1", mode], mode)
        self.assertEqual(value.quiesce.call_count, 3)
        module.dispatch(value, ["rollback-v1", "2.8.14", "/owned/recovery"], "baseline285")
        value.rollback_profile.assert_called_once_with("2.8.14", "/owned/recovery")
        value.activate.assert_called_with("original")
        module.dispatch(value, ["quiesce-v1", "baseline285", "rollback"], "baseline285")
        value.quiesce.assert_called_with(rollback=True)
        for argv, mode in (([], "unknown"), (["quiesce-v1", "fresh"], "baseline285"), (["rollback-v1"], "upgrade")):
            with self.subTest(argv=argv, mode=mode), self.assertRaises(module.Refusal):
                module.dispatch(value, argv, mode)

    def test_main_baseline_binding_comes_only_from_protected_spec_not_default_environment(self):
        import os
        module, value, _, _ = self.provider()
        spec = value.s
        spec.update(contract="punto-installed-runtime-v1", domain_uuid="e5926030-f735-4637-936c-4af8ee226f70",
            lifecycle_mode="baseline285", callback_budget_seconds=10, facts_module="/protected/runtime_facts.py",
            exact285_archive="/protected/285.deb", candidate_archive="/protected/candidate.deb",
            files_helper={"path": "/protected/files.sh", "sha256": "a" * 64})
        spec["dependencies"] = {str(Path(module.__file__)): "a" * 64, "/protected/runtime_facts.py": "a" * 64,
                                "/protected/candidate.deb": "a" * 64, "/protected/files.sh": "a" * 64}
        spec["profiles"]["exact285"] = {"version": "2.8.5", "processes": {
            "daemon": "/usr/local/bin/punto-daemon", "tray": "/usr/local/bin/punto-tray", "udevmon": "/usr/bin/udevmon",
            "interception": "/usr/bin/intercept", "uinput": "/usr/bin/uinput"},
            "installed": {"/usr/local/bin/punto-daemon": "edc37fc25bd16d79e8a1252d1e8a9ef012099b271f8c8b6e7f17176d173cd6a6",
                "/usr/local/bin/punto": "07b56664729be21303560f7912028e0c64e056b375573dcfd21d92875e3b4579",
                "/usr/local/bin/punto-tray": "45881738bda0433702bcbe2bc211e07f343f109360a2a75840a43676cddf5db1"}}
        spec["profiles"]["exact285"]["installed"].update({"/usr/bin/udevmon": "a" * 64,
                                                          "/usr/bin/intercept": "a" * 64, "/usr/bin/uinput": "a" * 64})
        value.activate = Mock(return_value={"runtime_verified": True, "input_verified": False})
        def read(path, digest=None):
            return json.dumps(spec).encode() if path == "/protected/spec.json" else b""
        with patch.object(module.os, "geteuid", return_value=0), patch.object(module, "trusted_file", side_effect=read), \
                patch.object(module, "Provider", return_value=value), patch("builtins.print"), \
                patch.dict(os.environ, {"PUNTO_PROVIDER_SPEC": "/protected/spec.json"}, clear=True):
            self.assertEqual(module.main([]), 0)
        value.activate.assert_called_once_with("exact285")

    @staticmethod
    def provider():
        import importlib.util
        path = Path(__file__).resolve().parents[2] / "deployment/punto-installed-lifecycle-provider.py"
        loader = importlib.util.spec_from_file_location("cached_lifecycle_provider", path)
        module = importlib.util.module_from_spec(loader)
        loader.loader.exec_module(module)
        profile = {"version": "2.8.14", "processes": {"udevmon": "/usr/bin/udevmon", "daemon": "/usr/bin/punto-daemon",
            "interception": "/usr/bin/intercept", "uinput": "/usr/bin/uinput", "helper": "/usr/libexec/punto-session-bridge",
            "tray": "/usr/bin/punto-tray"}}
        profile["installed"] = {path: "a" * 64 for path in profile["processes"].values()}
        spec = {"uid": 1000, "gid": 1000, "username": "punto", "home": "/home/punto", "session": "7",
                "display": ":0", "xauthority": "/run/user/1000/gdm/Xauthority", "original_enabled": False,
                "profiles": {"original": profile, "candidate": profile}, "evdev": "/dev/input/by-id/owned-kbd",
                "core": {"gnome": {"identity": [20, "200"]}}, "watcher_owner": ":1.20",
                "legacy_tray_unit": "punto-legacy-tray-" + "a" * 32 + ".service"}
        facts = Mock()
        records = {index: {"executable": path, "deleted": False, "sha256": "a" * 64,
            "uid": 1000 if role in ("helper", "tray") else 0, "identity": [index, str(index * 10)]}
            for index, (role, path) in enumerate(profile["processes"].items(), 10)}
        spec.update(initial_profile="original", initial_writers={role: records[index]["identity"]
                    for index, role in enumerate(profile["processes"], 10)})
        spec["initial_profile"] = "candidate"
        facts.pin_process.side_effect = lambda pid, path, identity: (identity, pid + 100)
        value = module.Provider(spec, facts, 10, clock=lambda: 0)
        value.unit_authority = Mock()
        return module, value, profile, records

    def test_trusted_read_ignores_access_time_but_never_inode_or_contents_drift(self):
        from types import SimpleNamespace
        import hashlib
        module, _, _, _ = self.provider()
        payload = b"admitted"
        fields = dict(st_dev=1, st_ino=2, st_mode=0o100644, st_uid=0, st_gid=0, st_nlink=1,
                      st_size=len(payload), st_mtime_ns=3, st_ctime_ns=4, st_atime_ns=5)
        for defect in (None, "st_ino", "st_mtime_ns", "st_ctime_ns"):
            before = SimpleNamespace(**fields)
            after = SimpleNamespace(**{**fields, "st_atime_ns": 99, **({defect: 100} if defect else {})})
            file = Mock(parents=[], **{"is_absolute.return_value": True, "read_bytes.return_value": payload,
                "lstat.side_effect": [before, before, after]})
            with self.subTest(defect=defect), patch.object(module, "Path", return_value=file):
                if defect:
                    with self.assertRaises(module.Refusal):
                        module.trusted_file("/owned/file", hashlib.sha256(payload).hexdigest())
                else:
                    self.assertEqual(module.trusted_file("/owned/file", hashlib.sha256(payload).hexdigest()), payload)

    def test_unit_job_and_fragment_drift_are_never_runtime_readiness(self):
        module, value, profile, _ = self.provider()
        value.run = Mock(return_value=json.dumps({"LoadState": "loaded", "ActiveState": "active", "MainPID": 10,
            "FragmentPath": "/usr/lib/systemd/system/udevmon.service", "DropInPaths": [], "Job": [42, "/owned/job"]}))
        with self.assertRaises(module.Refusal) as raised:
            value.unit("udevmon.service")
        self.assertEqual(raised.exception.status, 75)
        # Exercise the actual authority function, not the mechanical factory override.
        with self.assertRaises(module.Refusal):
            module.Provider.unit_authority(value, "punto-tray.service", {"LoadState": "loaded",
                "FragmentPath": "/etc/systemd/user/foreign.service", "DropInPaths": []}, profile)
        with self.assertRaises(module.Refusal):
            module.Provider.unit_authority(value, "punto-tray.service", {"LoadState": "loaded",
                "FragmentPath": "/usr/lib/systemd/user/punto-tray.service",
                "DropInPaths": ["/run/user/1000/systemd/user/punto-tray.service.d/foreign.conf"]}, profile)

    def test_unit_observation_refs_inactive_object_on_one_authenticated_connection(self):
        from gi.repository import Gio, GLib
        for load, active, main_pid in (("loaded", "inactive", 0), ("loaded", "active", 15),
                                       ("not-found", "inactive", 0), ("masked", "inactive", 0)):
            for user in (False, True):
                module, value, _, _ = self.provider()
                clock, calls, pinned = [0], [], [False]
                value.clock = lambda: clock[0]
                connection = Mock()
                def call(destination, path, interface, method, parameters, signature, flags, timeout, cancellable):
                    calls.append((destination, interface, method, timeout))
                    clock[0] += .1
                    if method == "GetNameOwner":
                        return GLib.Variant("(s)", (":1.20",))
                    if method == "GetConnectionUnixUser":
                        return GLib.Variant("(u)", (1000 if user else 0,))
                    if method == "LoadUnit":
                        return GLib.Variant("(o)", ("/org/freedesktop/systemd1/unit/owned",))
                    if method == "Ref":
                        pinned[0] = True
                        return GLib.Variant("()", ())
                    if method == "GetAll":
                        if not pinned[0]:
                            raise module.Refusal("NoSuchUnit: inactive unit was garbage collected")
                        props = {"MainPID": GLib.Variant("u", main_pid)} if parameters.unpack()[0].endswith(".Service") else {
                            "Id": GLib.Variant("s", "udevmon.service"),
                            "LoadState": GLib.Variant("s", load), "ActiveState": GLib.Variant("s", active),
                            "FragmentPath": GLib.Variant("s", "/run/systemd/system/udevmon.service" if load == "masked" else "/unit"),
                            "DropInPaths": GLib.Variant("as", []), "Job": GLib.Variant("(uo)", (0, "/"))}
                        return GLib.Variant("(a{sv})", (props,))
                    raise AssertionError(method)
                connection.call_sync.side_effect = call
                def run(argv):
                    if "-c" in argv:
                        with patch.object(module.sys, "argv", ["unit-read", *argv[-3:]]), \
                                patch.object(module.time, "monotonic", side_effect=value.clock), patch("builtins.print") as output:
                            exec(argv[argv.index("-c") + 1], {})
                        return output.call_args.args[0]
                    return {"LoadState": load, "ActiveState": active, "MainPID": str(main_pid),
                            "FragmentPath": "/unit"}[argv[-2].split("=", 1)[1]]
                value.run = Mock(side_effect=run)
                value.bus = Mock(side_effect=module.Refusal("NoSuchUnit: disconnected manager read"))
                with self.subTest(load=load, user=user), patch.object(module.os, "getuid", return_value=1000 if user else 0), \
                        patch.object(module, "__file__", "/root/protected0700/provider.py"), \
                        patch.object(module, "Path", side_effect=PermissionError("user cannot read staged callback")), \
                        patch.object(Gio, "dbus_address_get_for_bus_sync", return_value="unix:path=/original/bus"), \
                        patch.object(Gio.DBusConnection, "new_for_address_sync", return_value=connection):
                    observed = value.unit("udevmon.service", user)
                self.assertEqual((observed["LoadState"], observed["ActiveState"], observed["MainPID"]), (load, active, main_pid))
                self.assertEqual([call[2] for call in calls], ["GetNameOwner", "GetConnectionUnixUser", "LoadUnit", "Ref", "GetAll", "GetAll", "GetAll", "GetNameOwner"])
                self.assertTrue(all(left[3] >= right[3] for left, right in zip(calls, calls[1:])))
                self.assertEqual(value.run.call_count, 1)
                value.bus.assert_not_called()
                connection.close_sync.assert_called_once_with(None)
                argv = value.run.call_args.args[0]
                self.assertEqual("/usr/bin/setpriv" in argv, user)
                self.assertEqual(argv[-1], str(value.deadline))
                self.assertNotIn("/root/protected0700/provider.py", argv)

    def test_unit_observation_rejects_context_handoff_schema_and_deadline_and_releases_reference(self):
        from gi.repository import Gio, GLib
        for defect in ("context", "manager-uid", "manager-owner", "handoff", "path", "id", "type", "job", "dropin", "changed-state", "ref", "transport", "expired"):
            module, value, profile, _ = self.provider()
            clock, reads, owners = [0], [0], [0]
            value.clock = lambda: clock[0]
            connection = Mock()
            def call(destination, path, interface, method, parameters, signature, flags, timeout, cancellable):
                clock[0] += .1
                if defect == "expired":
                    clock[0] = value.deadline + .1
                if method == "GetNameOwner":
                    owners[0] += 1
                    return GLib.Variant("(s)", ("foreign-name" if defect == "manager-owner" else
                        ":1.21" if defect == "handoff" and owners[0] == 2 else ":1.20",))
                if method == "GetConnectionUnixUser":
                    return GLib.Variant("(u)", (1000 if defect == "manager-uid" else 0,))
                if method == "LoadUnit":
                    return GLib.Variant("(o)", ("/foreign/path" if defect == "path" else "/org/freedesktop/systemd1/unit/owned",))
                if method == "Ref":
                    if defect == "ref":
                        raise OSError("reference refused")
                    return GLib.Variant("()", ())
                if method == "GetAll":
                    if defect == "transport":
                        raise OSError("manager disconnected")
                    if parameters.unpack()[0].endswith(".Service"):
                        return GLib.Variant("(a{sv})", ({"MainPID": GLib.Variant("b", False) if defect == "type" else GLib.Variant("u", 0)},))
                    reads[0] += 1
                    return GLib.Variant("(a{sv})", ({"Id": GLib.Variant("s", "foreign.service" if defect == "id" else "udevmon.service"),
                        "LoadState": GLib.Variant("s", "loaded"), "ActiveState": GLib.Variant("s", "active" if defect == "changed-state" and reads[0] == 2 else "inactive"),
                        "FragmentPath": GLib.Variant("s", "/unit"), "DropInPaths": GLib.Variant("as", ["/foreign/dropin"] if defect == "dropin" else []),
                        "Job": GLib.Variant("(uo)", (42, "/job") if defect == "job" else (0, "/"))},))
                raise AssertionError(method)
            connection.call_sync.side_effect = call
            value.run = Mock(side_effect=lambda argv: json.dumps(module.observe_unit("udevmon.service", False, value.deadline, clock=value.clock)))
            with self.subTest(defect=defect), patch.object(module.os, "getuid", return_value=1000 if defect == "context" else 0), \
                    patch.object(Gio, "dbus_address_get_for_bus_sync", return_value="unix:path=/original/bus"), \
                    patch.object(Gio.DBusConnection, "new_for_address_sync", return_value=connection) as connect:
                with self.assertRaises((module.Refusal, RuntimeError, OSError)) as refused:
                    observed = value.unit("udevmon.service")
                    module.Provider.unit_authority(value, "udevmon.service", observed, profile)
                if defect == "job":
                    self.assertEqual(refused.exception.status, 75)
                if defect == "context":
                    connect.assert_not_called()
                    connection.close_sync.assert_not_called()
                else:
                    connection.close_sync.assert_called_once_with(None)

    def test_runtime_mask_fragment_projection_reuses_orchestrator_authority(self):
        module, value, profile, _ = self.provider()
        for fragment in ("/run/systemd/system/udevmon.service", "/dev/null"):
            with self.subTest(fragment=fragment), patch.object(module, "trusted_file") as read:
                module.Provider.unit_authority(value, "udevmon.service", {"LoadState": "masked",
                    "FragmentPath": fragment, "DropInPaths": []}, profile)
                read.assert_not_called()
        for unit, load, fragment, dropins in (
                ("udevmon.service", "masked", "/etc/systemd/system/udevmon.service", []),
                ("udevmon.service", "masked", "/run/systemd/system/foreign.service", []),
                ("punto-tray.service", "masked", "/run/systemd/system/udevmon.service", []),
                ("udevmon.service", "loaded", "/run/systemd/system/udevmon.service", []),
                ("udevmon.service", "loaded", "/dev/null", []),
                ("udevmon.service", "masked", "/run/systemd/system/udevmon.service", ["/foreign/dropin.conf"]),
                ("udevmon.service", "masked", "/dev/null", ["/foreign/dropin.conf"])):
            with self.subTest(unit=unit, load=load, fragment=fragment, dropins=dropins), self.assertRaises(module.Refusal):
                module.Provider.unit_authority(value, unit, {"LoadState": load,
                    "FragmentPath": fragment, "DropInPaths": dropins}, profile)

    def test_spawn_time_cannot_extend_original_command_deadline(self):
        module, value, _, _ = self.provider()
        clock = [0]
        value.clock = lambda: clock[0]
        child = Mock(returncode=0, **{"communicate.return_value": (b"done", b"")})
        def spawn(*args, **kwargs):
            clock[0] = 7
            return child
        with patch.object(module.subprocess, "Popen", side_effect=spawn):
            self.assertEqual(value.run(["/usr/bin/systemctl", "show", "udevmon.service"]), "done")
        child.communicate.assert_called_once_with(timeout=3)
        clock[0] = 0
        def late_spawn(*args, **kwargs):
            clock[0] = 11
            return child
        child.reset_mock()
        with patch.object(module.subprocess, "Popen", side_effect=late_spawn), self.assertRaises(module.Refusal) as raised:
            value.run(["/usr/bin/systemctl", "start", "udevmon.service"])
        self.assertEqual(raised.exception.status, 75)
        child.kill.assert_called_once()

    def test_successor_unit_foreign_effective_owner_refuses_before_start(self):
        module, value, profile, _ = self.provider()
        value.unit = Mock(return_value={"LoadState": "loaded", "ActiveState": "inactive", "MainPID": 0,
            "FragmentPath": "/foreign/punto-tray.service", "DropInPaths": []})
        value.unit_authority = lambda unit, state, profile: module.Provider.unit_authority(value, unit, state, profile)
        value.control = Mock()
        with self.assertRaises(module.Refusal):
            value.start("punto-tray.service", profile, user=True)
        value.control.assert_not_called()

    def test_quiesce_pins_all_before_any_stop_and_confirms_death(self):
        module, value, profile, records = self.provider()
        events = []
        value.authority = Mock(side_effect=lambda: events.append("authority"))
        value.payload, value.current_profile = Mock(return_value=profile), Mock(return_value="candidate")
        value.f.writer_processes.side_effect = [records, {}]
        value.f.pin_process.side_effect = lambda pid, path, identity: (events.append(("pin", pid)) or (identity, pid + 100))
        value.tray_owner = Mock(return_value=":1.2")
        value.unit = Mock(side_effect=lambda unit, user=False: {"LoadState": "loaded", "ActiveState": "active",
            "MainPID": {"udevmon.service": 10, "punto-session-bridge.service": 14, "punto-tray.service": 15}.get(unit, 0)})
        value.run = Mock(side_effect=lambda argv: events.append(("command", argv)) or "")
        with patch.object(module.select, "select", side_effect=lambda reads, *_: (reads, [], [])), \
                patch.object(module.os, "close") as close:
            result = value.quiesce()
        self.assertEqual(result["writers_dead"], 6)
        self.assertFalse(result["input_verified"])
        first_stop = next(index for index, event in enumerate(events) if isinstance(event, tuple) and event[0] == "command")
        self.assertEqual(sum(isinstance(event, tuple) and event[0] == "pin" for event in events[:first_stop]), 6)
        self.assertEqual(close.call_count, 6)

    def test_quiesce_foreign_writer_or_unit_never_signals(self):
        import copy
        for defect in ("deleted", "sha256", "uid", "competing", "unit"):
            module, value, profile, records = self.provider()
            value.authority, value.payload, value.current_profile = Mock(), Mock(return_value=profile), Mock(return_value="candidate")
            modified = copy.deepcopy(records)
            if defect == "competing":
                modified[99] = dict(modified[10], identity=[99, "990"])
            elif defect != "unit":
                modified[10][defect] = True if defect == "deleted" else 1000 if defect == "uid" else "b" * 64
            value.f.writer_processes.return_value = modified
            value.unit = Mock(return_value={"LoadState": "loaded", "MainPID": 999})
            value.control, value.tray_owner = Mock(), Mock()
            with self.subTest(defect=defect), patch.object(module.os, "close"), \
                    patch.object(module.signal, "pidfd_send_signal") as send, self.assertRaises(module.Refusal):
                value.quiesce()
            value.control.assert_not_called()
            send.assert_not_called()

    def test_initial_same_elf_restarted_pid_never_admits_an_unapproved_instance(self):
        module, value, profile, records = self.provider()
        records[10]["identity"] = [10, "999"]
        value.authority, value.payload, value.current_profile = Mock(), Mock(return_value=profile), Mock(return_value="candidate")
        value.f.writer_processes.return_value = records
        value.unit, value.control = Mock(), Mock()
        with self.assertRaises(module.Refusal):
            value.quiesce()
        value.f.pin_process.assert_not_called()
        value.unit.assert_not_called()
        value.control.assert_not_called()

    def test_quiesce_empty_and_respawn_are_not_legacy_fallback(self):
        for records in ({}, {99: {"executable": "/foreign/punto-daemon"}}):
            module, value, profile, _ = self.provider()
            value.authority, value.payload, value.current_profile = Mock(), Mock(return_value=profile), Mock(return_value="candidate")
            value.f.writer_processes.side_effect = [{}, records]
            value.f.pin_process.side_effect = ProcessLookupError()
            value.unit = Mock(return_value={"LoadState": "not-found", "MainPID": 0})
            value.control = Mock()
            if records:
                with self.assertRaises(module.Refusal):
                    value.quiesce()
            else:
                self.assertEqual(value.quiesce()["writers_dead"], 0)
            value.control.assert_not_called()
            self.assertEqual(value.f.pin_process.call_count, 6)

    def test_same_payload_rollback_readmits_only_managed_new_writers(self):
        module, value, profile, records = self.provider()
        value.authority, value.payload, value.current_profile = Mock(), Mock(return_value=profile), Mock(return_value="candidate")
        records[10]["identity"] = [10, "new-start"]
        value.f.writer_processes.side_effect = [records, {}]
        value.tray_owner, value.control = Mock(), Mock()
        value.unit = Mock(side_effect=lambda unit, user=False: {"LoadState": "loaded", "MainPID":
            {"udevmon.service": 10, "punto-session-bridge.service": 14, "punto-tray.service": 15}.get(unit, 0)})
        with patch.object(module.select, "select", side_effect=lambda reads, *_: (reads, [], [])), patch.object(module.os, "close"):
            self.assertEqual(value.quiesce(rollback=True)["writers_dead"], 6)
        value.f.writer_processes.side_effect = None
        value.f.writer_processes.return_value = records
        value.unit.return_value = {"LoadState": "loaded", "MainPID": 0}
        value.unit.side_effect = None
        with patch.object(module.os, "close"), self.assertRaises(module.Refusal):
            value.quiesce(rollback=True)

    def test_empty_writer_scan_cannot_hide_still_live_initial_identity(self):
        module, value, profile, _ = self.provider()
        value.authority, value.payload, value.current_profile = Mock(), Mock(return_value=profile), Mock(return_value="candidate")
        value.f.writer_processes.return_value = {}
        value.unit, value.control = Mock(), Mock()
        with patch.object(module.os, "close") as close, self.assertRaises(module.Refusal) as raised:
            value.quiesce()
        self.assertEqual(raised.exception.status, 75)
        close.assert_called_once()
        value.unit.assert_not_called()
        value.control.assert_not_called()

    def test_orchestrator_is_the_only_explicit_rollback_phase_caller(self):
        path = Path(__file__).resolve().parents[2] / "deployment/punto-activation-orchestrator.sh"
        text = path.read_text()
        self.assertEqual(text.count("quiesce_writers rollback"), 1)
        self.assertIn('"$quiesce" quiesce-v1 "$lifecycle_mode" "$@"', text)
        self.assertIn("rollback_package()", text.split("quiesce_writers rollback", 1)[0])

    def test_command_timeout_reaps_control_and_never_resends(self):
        module, value, _, _ = self.provider()
        child = Mock(returncode=0)
        child.communicate.side_effect = [subprocess.TimeoutExpired(["systemctl"], 10), (b"", b"")]
        with patch.object(module.subprocess, "Popen", return_value=child) as launch, self.assertRaises(module.Refusal) as raised:
            value.run(["/usr/bin/systemctl", "start", "udevmon.service"])
        self.assertEqual(raised.exception.status, 75)
        self.assertEqual(launch.call_count, 1)
        child.kill.assert_called_once()
        self.assertNotIn("start_new_session", launch.call_args.kwargs)

    def test_expiry_before_command_and_profile_never_turns_unknown_into_mismatch(self):
        module, value, _, _ = self.provider()
        value.clock = lambda: 11
        with patch.object(module.subprocess, "Popen") as launch, self.assertRaises(module.Refusal) as raised:
            value.run(["/usr/bin/systemctl", "stop", "udevmon.service"])
        self.assertEqual(raised.exception.status, 75)
        launch.assert_not_called()
        value.payload = Mock(side_effect=module.Refusal("expired", 75))
        with self.assertRaises(module.Refusal) as raised:
            value.current_profile()
        self.assertEqual(raised.exception.status, 75)
        value.payload.assert_called_once()

    def test_effect_rechecks_authority_before_and_after_single_dispatch(self):
        module, value, _, _ = self.provider()
        events = []
        value.authority = Mock(side_effect=lambda: events.append("authority"))
        value.run = Mock(side_effect=lambda _: events.append("effect"))
        value.effect(["/usr/bin/systemctl", "stop", "udevmon.service"])
        self.assertEqual(events, ["authority", "effect", "authority"])
        value.authority.side_effect = module.Refusal("core drift")
        with self.assertRaises(module.Refusal):
            value.effect(["/usr/bin/systemctl", "stop", "udevmon.service"])
        self.assertEqual(value.run.call_count, 1)

    def test_pidfd_death_timeout_and_controller_interruption_remain_unknown(self):
        module, value, profile, records = self.provider()
        value.authority, value.payload, value.current_profile = Mock(), Mock(return_value=profile), Mock(return_value="candidate")
        value.f.writer_processes.return_value = records
        value.unit = Mock(return_value={"LoadState": "loaded", "MainPID": 0})
        value.tray_owner, value.control = Mock(), Mock()
        with patch.object(module.select, "select", return_value=([], [], [])), \
                patch.object(module.signal, "pidfd_send_signal") as send, patch.object(module.os, "close") as close, \
                self.assertRaises(module.Refusal) as raised:
            value.quiesce()
        self.assertEqual(raised.exception.status, 75)
        self.assertEqual(close.call_count, 6)
        send.assert_called_once_with(115, module.signal.SIGTERM)
        with self.assertRaises(module.Refusal) as interrupted:
            module.interrupted(module.signal.SIGTERM, None)
        self.assertEqual(interrupted.exception.status, 75)

    def test_admitted_effect_post_authority_failure_preserves_unknown_once(self):
        for failure in ("refusal", "transport", "malformed"):
            module, value, _, _ = self.provider()
            error = module.Refusal("authority changed", 66) if failure == "refusal" else \
                    OSError("authority transport lost") if failure == "transport" else ValueError("authority observation malformed")
            value.authority = Mock(side_effect=[None, error])
            value.run = Mock(return_value="completed command reply")
            argv = ["/usr/bin/systemctl", "start", "udevmon.service"]
            with self.subTest(failure=failure), self.assertRaises(Exception) as raised:
                value.effect(argv)
            with self.subTest(failure=failure):
                self.assertIsInstance(raised.exception, module.Refusal)
                self.assertEqual(raised.exception.status, 75)
                self.assertTrue(value.pending_effect)
                self.assertEqual(value.authority.call_count, 2)
                value.run.assert_called_once_with(argv)

    def test_effect_precheck_and_success_then_pure_refusal_remain_known(self):
        module, value, _, _ = self.provider()
        value.authority = Mock(side_effect=module.Refusal("not admitted", 66))
        value.run = Mock(return_value="completed command reply")
        argv = ["/usr/bin/systemctl", "start", "udevmon.service"]
        with self.assertRaises(module.Refusal) as before:
            value.effect(argv)
        self.assertEqual(before.exception.status, 66)
        self.assertFalse(value.pending_effect)
        value.run.assert_not_called()
        value.authority.side_effect = None
        value.authority.reset_mock()
        value.effect(argv)
        self.assertFalse(value.pending_effect)
        self.assertEqual(value.authority.call_count, 2)
        value.run.assert_called_once_with(argv)
        value.authority.side_effect = module.Refusal("later observation refused", 66)
        with self.assertRaises(module.Refusal) as after:
            value.authority()
        self.assertEqual(after.exception.status, 66)
        self.assertFalse(value.pending_effect)
        value.run.assert_called_once_with(argv)

    def test_tray_owner_uses_actual_unix_pid_and_rejects_ambiguity(self):
        module, value, _, _ = self.provider()
        value.bus = Mock(side_effect=lambda destination, path, method, arguments=(), **kwargs:
                         (":1.20",) if method.endswith("GetNameOwner") else
                         ([":1.9/StatusNotifierItem", ":1.10/StatusNotifierItem"],))
        value.bus_peer = Mock(side_effect=[(":1.20", 20, 1000), (":1.9", 15, 1000), (":1.10", 99, 1000)])
        self.assertEqual(value.tray_owner(15), ":1.9")
        for peers in ([ (":1.9", 15, 0), (":1.10", 99, 1000) ],
                      [ (":1.9", 15, 1000), (":1.10", 15, 1000) ]):
            value.bus_peer.side_effect = [(":1.20", 20, 1000), *peers]
            with self.assertRaises(module.Refusal):
                value.tray_owner(15)

    def test_tray_owner_accepts_gnome_and_standard_catalogues_with_authenticated_peers(self):
        for separator in ("@/", "/"):
            module, value, _, _ = self.provider()
            catalogue = [":1.96" + separator + "org/ayatana/NotificationItem/punto_switcher",
                         ":1.98" + separator + "org/ayatana/NotificationItem/software_update_available",
                         ":1.98" + separator + "org/ayatana/NotificationItem/livepatch"]
            peers = {"org.kde.StatusNotifierWatcher": (":1.20", 20, 1000),
                     ":1.96": (":1.96", 15, 1000), ":1.98": (":1.98", 99, 1000)}
            def peer(name):
                if name not in peers:
                    raise module.Refusal("DBus NameHasNoOwner: " + name)
                return peers[name]
            value.bus_peer = Mock(side_effect=peer)
            value.bus = Mock(side_effect=lambda destination, path, method, arguments=(), **kwargs:
                             (":1.20",) if method.endswith("GetNameOwner") else (catalogue,))
            with self.subTest(separator=separator):
                self.assertEqual(value.tray_owner(15), ":1.96")
                self.assertEqual([call.args[0] for call in value.bus_peer.call_args_list],
                                 ["org.kde.StatusNotifierWatcher", ":1.96", ":1.98", ":1.98"])

    def test_tray_catalogue_rejects_malformed_duplicate_and_ambiguous_entries(self):
        own = ":1.96@/org/ayatana/NotificationItem/punto_switcher"
        foreign = ":1.98@/org/ayatana/NotificationItem/livepatch"
        for defect in ("no-path", "bad-name", "bad-path", "duplicate-foreign", "duplicate-form", "ambiguous", "foreign-uid", "foreign-pid", "transport"):
            module, value, _, _ = self.provider()
            malformed = {"no-path": ":1.98", "bad-name": ":1.98@@/Item", "bad-path": ":1.98@/invalid-path"}
            catalogue = [own]
            if defect in malformed:
                catalogue.append(malformed[defect])
            elif defect == "duplicate-foreign":
                catalogue.extend([foreign, foreign])
            elif defect == "duplicate-form":
                catalogue.extend([foreign, foreign.replace("@/", "/")])
            elif defect == "ambiguous":
                catalogue.append(foreign)
            def peer(name):
                if name == "org.kde.StatusNotifierWatcher":
                    return ":1.20", 20, 1000
                if defect == "transport":
                    raise OSError("bus disconnected")
                return name, 99 if defect == "foreign-pid" else 15, 0 if defect == "foreign-uid" else 1000
            value.bus_peer = Mock(side_effect=peer)
            value.bus = Mock(side_effect=lambda destination, path, method, arguments=(), **kwargs:
                             (":1.20",) if method.endswith("GetNameOwner") else (catalogue,))
            with self.subTest(defect=defect), self.assertRaises(OSError if defect == "transport" else module.Refusal):
                value.tray_owner(15)

    def test_publication_peer_handoff_never_authorizes_old_unique_owner(self):
        module, value, _, _ = self.provider()
        value.bus = Mock(side_effect=[(":1.9",), (15,), (1000,), (":1.10",)])
        with self.assertRaises(module.Refusal):
            value.bus_peer("org.punto.Switcher.SessionBridge1")

    def test_watcher_observation_uses_pinned_unique_core_owner_never_autoactivation(self):
        _, value, _, _ = self.provider()
        value.s.update(core={"gnome": {"identity": [20, "200"]}}, watcher_owner=":1.20")
        value.bus_peer = Mock(side_effect=lambda name: (":1.20", 20, 1000)
                             if name == "org.kde.StatusNotifierWatcher" else (":1.9", 15, 1000))
        value.bus = Mock(side_effect=lambda destination, path, method, arguments=(), **kwargs:
                         (":1.20",) if method.endswith("GetNameOwner") else ([":1.9/StatusNotifierItem"],))
        self.assertEqual(value.tray_owner(15), ":1.9")
        observations = [call for call in value.bus.call_args_list if call.args[2].endswith("Properties.Get")]
        self.assertEqual([call.args[0] for call in observations], [":1.20"])

    def test_type_exec_publication_has_one_bounded_readonly_settling_owner(self):
        module, value, _, _ = self.provider()
        self.assertTrue(callable(getattr(value, "await_publication", None)),
                        "Type=exec start has no positive not-yet-published settling boundary")
        self.assertTrue(issubclass(getattr(module, "NotPublished", object), module.Refusal),
                        "startup absence has no separate typed classification")

    def test_publication_wait_accepts_delayed_valid_without_restarting_successors(self):
        module, value, profile, records = self.provider()
        self.assertTrue(callable(getattr(value, "await_publication", None)))
        identity = {role: records[pid]["identity"] for role, pid in zip(profile["processes"], records)}
        value.authority = Mock()
        value.f.writer_processes.return_value = records
        value.unit = Mock(side_effect=lambda unit, user=False: {"LoadState": "loaded", "ActiveState": "active",
            "MainPID": {"udevmon.service": 10, "punto-session-bridge.service": 14, "punto-tray.service": 15}[unit]})
        value.helper = Mock(side_effect=[module.NotPublished("descriptor absent"), ":1.14", ":1.14"])
        value.tray_owner = Mock(side_effect=[module.NotPublished("not registered yet"), ":1.15"])
        value.control, value.effect = Mock(), Mock()
        with patch.object(module.time, "sleep") as sleep:
            self.assertEqual(value.await_publication(profile, identity, False), (":1.14", ":1.15"))
        self.assertEqual(sleep.call_count, 2)
        self.assertGreaterEqual(value.authority.call_count, 6)
        value.control.assert_not_called()
        value.effect.assert_not_called()

    def test_publication_wait_refuses_malformed_drift_transport_and_original_expiry(self):
        for defect in ("malformed", "transport", "unit", "writer", "expired"):
            module, value, profile, records = self.provider()
            self.assertTrue(callable(getattr(value, "await_publication", None)))
            identity = {role: records[pid]["identity"] for role, pid in zip(profile["processes"], records)}
            value.authority = Mock()
            value.f.writer_processes.return_value = records
            value.unit = Mock(side_effect=lambda unit, user=False: {"LoadState": "loaded", "ActiveState": "active",
                "MainPID": {"udevmon.service": 10, "punto-session-bridge.service": 14, "punto-tray.service": 15}[unit]})
            value.helper, value.tray_owner = Mock(return_value=":1.14"), Mock(return_value=":1.15")
            value.control, value.effect = Mock(), Mock()
            clock = [0]
            value.clock = lambda: clock[0]
            if defect == "malformed":
                value.helper.side_effect = module.Refusal("descriptor malformed")
            elif defect == "transport":
                value.helper.side_effect = OSError("bus disconnected")
            elif defect == "unit":
                value.unit.side_effect = lambda *_args, **_kwargs: {"LoadState": "loaded", "ActiveState": "failed", "MainPID": 0}
            elif defect == "writer":
                records[10]["identity"] = [10, "foreign-start"]
            else:
                value.helper.side_effect = module.NotPublished("descriptor absent")
            with self.subTest(defect=defect), patch.object(module.time, "sleep", side_effect=lambda _: clock.__setitem__(0, 11)) as sleep:
                with self.assertRaises((module.Refusal, OSError)) as raised:
                    value.await_publication(profile, identity, False)
                if defect == "expired":
                    self.assertEqual(raised.exception.status, 75)
                    self.assertEqual(sleep.call_count, 1)
                else:
                    sleep.assert_not_called()
            value.control.assert_not_called()
            value.effect.assert_not_called()

    def test_validated_helper_loss_or_handoff_cannot_become_initial_publication_wait(self):
        for successor in ("missing", "handoff"):
            module, value, profile, records = self.provider()
            identity = {role: records[pid]["identity"] for role, pid in zip(profile["processes"], records)}
            value.authority = Mock()
            value.f.writer_processes.return_value = records
            value.unit = Mock(side_effect=lambda unit, user=False: {"LoadState": "loaded", "ActiveState": "active",
                "MainPID": {"udevmon.service": 10, "punto-session-bridge.service": 14, "punto-tray.service": 15}[unit]})
            value.helper = Mock(side_effect=[":1.14", module.NotPublished("withdrawn") if successor == "missing" else ":1.16"])
            value.tray_owner = Mock(side_effect=module.NotPublished("not registered yet"))
            value.control, value.effect = Mock(), Mock()
            with self.subTest(successor=successor), patch.object(module.time, "sleep") as sleep:
                with self.assertRaises(module.Refusal) as raised:
                    value.await_publication(profile, identity, False)
                self.assertNotIsInstance(raised.exception, module.NotPublished)
                self.assertEqual(sleep.call_count, 1)
            value.control.assert_not_called()
            value.effect.assert_not_called()

    def test_watcher_handoff_or_foreign_core_is_fatal_not_publication_absence(self):
        for defect in ("catalogue-handoff", "foreign-core", "foreign-owner"):
            module, value, _, _ = self.provider()
            watcher = (":1.21", 20, 1000) if defect == "foreign-owner" else \
                      (":1.20", 99, 1000) if defect == "foreign-core" else (":1.20", 20, 1000)
            value.bus_peer = Mock(return_value=watcher)
            value.bus = Mock(side_effect=lambda destination, path, method, arguments=(), **kwargs:
                             (":1.21",) if method.endswith("GetNameOwner") else ([],))
            with self.subTest(defect=defect), self.assertRaises(module.Refusal) as raised:
                value.tray_owner(15)
            self.assertNotIsInstance(raised.exception, module.NotPublished)

    def test_catalogue_type_and_same_pid_foreign_uid_never_authorize_publication_wait(self):
        for catalogue in ("", (), None, [None], [""], [":1.9/StatusNotifierItem"]):
            module, value, profile, records = self.provider()
            identity = {role: records[pid]["identity"] for role, pid in zip(profile["processes"], records)}
            value.runtime_ready = Mock()
            value.helper = Mock(return_value=":1.14")
            value.bus_peer = Mock(side_effect=lambda name: (":1.20", 20, 1000)
                                 if name == "org.kde.StatusNotifierWatcher" else (":1.9", 15, 0))
            value.bus = Mock(side_effect=lambda destination, path, method, arguments=(), **kwargs:
                             (":1.20",) if method.endswith("GetNameOwner") else (catalogue,))
            value.control, value.effect = Mock(), Mock()
            with self.subTest(catalogue=catalogue), patch.object(module.time, "sleep") as sleep:
                # Bound the RED even if an invalid catalogue currently becomes pending.
                sleep.side_effect = module.Refusal("invalid pending reached wait", 75)
                with self.assertRaises(module.Refusal) as raised:
                    value.await_publication(profile, identity, False)
                self.assertNotIsInstance(raised.exception, module.NotPublished)
                sleep.assert_not_called()
            value.control.assert_not_called()
            value.effect.assert_not_called()

    def test_valid_empty_catalogue_waits_readonly_until_exact_tray_publication(self):
        module, value, profile, records = self.provider()
        identity = {role: records[pid]["identity"] for role, pid in zip(profile["processes"], records)}
        value.runtime_ready = Mock()
        value.helper = Mock(return_value=":1.14")
        catalogues = iter([[], [":1.15/StatusNotifierItem"]])
        value.bus_peer = Mock(side_effect=lambda name: (":1.20", 20, 1000)
                             if name == "org.kde.StatusNotifierWatcher" else (":1.15", 15, 1000))
        value.bus = Mock(side_effect=lambda destination, path, method, arguments=(), **kwargs:
                         (":1.20",) if method.endswith("GetNameOwner") else (next(catalogues),))
        value.control, value.effect = Mock(), Mock()
        with patch.object(module.time, "sleep") as sleep:
            self.assertEqual(value.await_publication(profile, identity, False), (":1.14", ":1.15"))
        sleep.assert_called_once_with(.025)
        self.assertEqual(value.runtime_ready.call_count, 4)
        value.control.assert_not_called()
        value.effect.assert_not_called()

    def test_helper_absence_only_is_pending_metadata_failure_is_fatal(self):
        from types import SimpleNamespace
        from unittest.mock import MagicMock
        module, value, _, _ = self.provider()
        self.assertTrue(hasattr(module, "NotPublished"))
        directory, descriptor = MagicMock(), MagicMock()
        directory.__truediv__.return_value = descriptor
        directory.lstat.return_value = SimpleNamespace(st_mode=0o040700, st_uid=1000)
        descriptor.lstat.side_effect = FileNotFoundError()
        with patch.object(module, "Path", return_value=directory), self.assertRaises(module.NotPublished):
            value.helper([14, "140"])
        descriptor.lstat.side_effect = None
        descriptor.lstat.return_value = SimpleNamespace(st_mode=0o100600, st_uid=0, st_nlink=1, st_size=128)
        with patch.object(module, "Path", return_value=directory), self.assertRaises(module.Refusal) as raised:
            value.helper([14, "140"])
        self.assertNotIsInstance(raised.exception, module.NotPublished)

    def test_legacy_tray_successor_is_one_nonce_managed_unit_not_modern_service(self):
        module, value, profile, records = self.provider()
        del profile["processes"]["helper"]
        profile["processes"]["daemon"] = "/usr/local/bin/punto-daemon"
        profile["processes"]["tray"] = "/usr/local/bin/punto-tray"
        del records[14]
        records[11]["executable"], records[15]["executable"] = "/usr/local/bin/punto-daemon", "/usr/local/bin/punto-tray"
        profile["installed"] = {path: "a" * 64 for path in profile["processes"].values()}
        value.authority, value.payload = Mock(), Mock(return_value=profile)
        snapshots = iter([{}, records])
        value.f.writer_processes.side_effect = lambda: next(snapshots, records)
        value.tray_owner = Mock(return_value=":1.7")
        calls = []
        value.run = Mock(side_effect=lambda argv: calls.append(argv) or "")
        running = [False]
        launched_tray = [False]
        def run(argv):
            calls.append(argv)
            if "/usr/bin/systemd-run" in argv:
                launched_tray[0] = True
            return ""
        value.run.side_effect = run
        value.start = Mock(side_effect=lambda *_args, **_kwargs: running.__setitem__(0, True))
        value.unit = Mock(side_effect=lambda unit, user=False: {
            "LoadState": "not-found" if unit == value.s["legacy_tray_unit"] and not launched_tray[0] else "loaded",
            "ActiveState": "active" if running[0] else "inactive",
            "MainPID": (10 if unit == "udevmon.service" else 15) if running[0] else 0})
        result = value.activate("exact285")
        launched = [argv for argv in calls if "/usr/bin/systemd-run" in argv]
        self.assertEqual(len(launched), 1)
        self.assertEqual(launched[0][-1], "/usr/local/bin/punto-tray")
        self.assertIn("--unit=" + value.s["legacy_tray_unit"], launched[0])
        self.assertFalse(any("punto-tray.service" in argv or "punto-session-bridge.service" in argv for argv in calls))
        self.assertEqual(result["profile"], "exact285")
        self.assertFalse(result["input_verified"])
        value.f.runtime_status.assert_called_once_with(False, 11, profile="exact285-gtk16", _deadline=10, failure=module.Refusal)

    def test_legacy_existing_unit_refuses_without_launch_and_no_writer_is_ever_reused(self):
        module, value, profile, _ = self.provider()
        value.authority, value.payload = Mock(), Mock(return_value=profile)
        value.f.writer_processes.return_value = {99: {"executable": "/usr/local/bin/punto-tray"}}
        value.effect = Mock()
        with self.assertRaises(module.Refusal):
            value.activate("exact285")
        value.effect.assert_not_called()
        value.f.writer_processes.return_value = {}
        value.unit = Mock(return_value={"LoadState": "loaded", "ActiveState": "inactive", "MainPID": 0})
        with self.assertRaises(module.Refusal):
            value.activate("exact285")
        self.assertFalse(any("/usr/bin/systemd-run" in call.args[0] for call in value.effect.call_args_list))

    def test_rollback_original_requires_sealed_raw_overlay_not_same_version_archive(self):
        module, value, profile, _ = self.provider()
        overlays = ("/usr/bin/punto-daemon", "/usr/libexec/punto-session-bridge",
                    "/usr/share/punto-switcher/protocol/punto-dbus-v1.xml", "/etc/xdg/autostart/punto-session-bridge.desktop")
        profile["installed"].update({path: "a" * 64 for path in overlays})
        value.s.update(files_helper={"path": "/usr/lib/punto-files", "sha256": "b" * 64},
                       exact285_archive="/owned/285.deb", candidate_archive="/owned/candidate.deb")
        value.run, value.payload = Mock(return_value="PUNTO_SNAPSHOT_VERIFIED files=sealed\n"), Mock(return_value=profile)
        with patch.object(module, "trusted_file", side_effect=lambda path, digest=None:
                          b"snapshot-original\n" if Path(path).name == "kind" else b"payload") as read:
            self.assertEqual(value.rollback_profile("2.8.14", "/owned/recovery.nonce"), "original")
        self.assertEqual([call.args[0] for call in read.call_args_list[2:]],
                         [Path("/owned/recovery.nonce/overlay") / path.lstrip("/") for path in overlays])
        with patch.object(module, "trusted_file", return_value=b"snapshot-original\n"), self.assertRaises(module.Refusal):
            value.rollback_profile("2.8.5", "/owned/recovery.nonce")

    def test_public_status_modern_provider_deadline_prevents_late_set_and_pins_peer(self):
        import struct
        import runtime_facts
        client = Mock(**{"recv.return_value": b"OK ENABLED\n"})
        client.getsockopt.return_value = struct.pack("3i", 42, 0, 0)
        context = Mock(__enter__=Mock(return_value=client), __exit__=Mock(return_value=False))
        clock = [0.0]
        client.connect.side_effect = lambda _: clock.__setitem__(0, 1.01)
        sockets = Mock(AF_UNIX=1, SOCK_STREAM=1, SOL_SOCKET=1, SO_PEERCRED=1)
        sockets.socket.return_value = context
        with self.assertRaises(runtime_facts.FactsFailure):
            runtime_facts.runtime_status(True, 42, _deadline=1, socket_module=sockets, clock=lambda: clock[0])
        client.sendall.assert_not_called()

    def test_shared_pidfd_pin_rechecks_incarnation_and_releases_rejected_fd(self):
        import runtime_facts
        expected = [42, "old-start"]
        with patch.object(runtime_facts, "process_identity", side_effect=[expected, [42, "new-start"]]), \
                patch.object(runtime_facts.os, "pidfd_open", return_value=90), \
                patch.object(runtime_facts.os, "close") as close, self.assertRaises(runtime_facts.FactsFailure):
            runtime_facts.pin_process(42, "/usr/bin/punto-daemon", expected)
        close.assert_called_once_with(90)
        with patch.object(runtime_facts, "process_identity", return_value=[42, "new-start"]), \
                patch.object(runtime_facts.os, "pidfd_open") as acquire, self.assertRaises(runtime_facts.FactsFailure):
            runtime_facts.pin_process(42, "/usr/bin/punto-daemon", expected)
        acquire.assert_not_called()

    def test_shared_actual_executable_observation_does_not_depend_on_argv(self):
        import hashlib
        import runtime_facts
        stat_line = "99 (unrelated argv) " + " ".join(["R", "1", *(["0"] * 17), "990"])
        item = Mock()
        item.parent.name = "99"
        item.parent.__truediv__ = Mock(return_value=Mock(**{"read_text.return_value": stat_line}))
        item.parent.stat.return_value.st_uid = 0
        item.read_bytes.return_value = b"ELF"
        root = Mock(**{"glob.return_value": [item]})
        def path(value):
            return root if value == "/proc" else Path(value)
        with patch.object(runtime_facts, "Path", side_effect=path), \
                patch.object(runtime_facts.os, "readlink", return_value="/foreign/punto-daemon (deleted)"):
            observed = runtime_facts.writer_processes()
        self.assertEqual(observed[99], {"executable": "/foreign/punto-daemon", "deleted": True,
            "uid": 0, "identity": [99, "990"], "sha256": hashlib.sha256(b"ELF").hexdigest()})

    def test_terminal_window_publication_is_conjunctive_bounded_and_owner_pinned(self):
        guest = Guest.__new__(Guest)
        guest.m = {"terminal": {"server": "/server"}}
        guest.terminal = {"app_id": "private", "owner": ":1.9", "title": "Punto acceptance abc123",
                          "identity": {"server": [100, "start"]}, "launcher": Mock(poll=Mock(return_value=None))}
        guest.terminal_bus = Mock(side_effect=lambda method, _name: {
            "GetNameOwner": ":1.9", "GetConnectionUnixProcessID": 100,
            "GetConnectionUnixUser": __import__("os").getuid()}[method])
        guest.terminal_process = Mock(return_value=[100, "start"])
        for mode in ("publish", "duplicate", "expiry", "foreign-owner", "changed-start", "fault"):
            with self.subTest(mode=mode):
                clock = [0.0]
                guest.terminal_bus.reset_mock()
                guest.terminal_process.return_value = [100, "changed" if mode == "changed-start" else "start"]
                values = [subprocess.CompletedProcess([], 1, b"", b""),
                          subprocess.CompletedProcess([], 0, b"42\n", b"")]
                if mode == "duplicate":
                    values = [subprocess.CompletedProcess([], 0, b"42\n43\n", b"")]
                if mode == "fault":
                    values = [subprocess.CompletedProcess([], 1, b"", b"X error")]
                if mode == "foreign-owner":
                    guest.terminal_bus.side_effect = lambda *_: ":foreign"
                else:
                    guest.terminal_bus.side_effect = lambda method, _name: {
                        "GetNameOwner": ":1.9", "GetConnectionUnixProcessID": 100,
                        "GetConnectionUnixUser": __import__("os").getuid()}[method]
                with patch("installed_guest.subprocess.run", side_effect=values) as run, \
                        patch("installed_guest.time.monotonic", side_effect=lambda: clock[0]), \
                        patch("installed_guest.time.sleep", side_effect=lambda _: clock.__setitem__(0, 5 if mode == "expiry" else .02)) as sleep:
                    if mode == "publish":
                        self.assertEqual(guest.terminal_window(100, 5), 42)
                        self.assertEqual(run.call_count, 2)
                        self.assertEqual(run.call_args.args[0], ["xdotool", "search", "--all", "--onlyvisible",
                            "--pid", "100", "--name", "^Punto acceptance abc123$"])
                    else:
                        with self.assertRaises(Failure):
                            guest.terminal_window(100, 5)
                        self.assertEqual(run.call_count, 0 if mode in ("foreign-owner", "changed-start") else 1)
                        if mode != "expiry":
                            sleep.assert_not_called()

    def test_shared_process_construction_preserves_partial_pins_and_fatal_guards(self):
        import runtime_facts
        owner = {"udevmon": "/usr/bin/udevmon", "interception": "/usr/bin/intercept", "helper": "/usr/bin/helper"}
        processes = {10: ([b"/usr/bin/udevmon"], 1, "root"), 20: ([b"/usr/bin/helper"], 1, "helper")}
        run = Mock(side_effect=lambda argv: "good hash" if argv[0] == "sha256sum" else owner[
            "udevmon" if "/10/" in argv[-1] else "helper" if "/20/" in argv[-1] else "interception"])
        with self.assertRaises(runtime_facts.ProcessesNotPublished) as pending:
            runtime_facts.process_identities(processes, owner, {}, run, privilege=(),
                                             not_yet_published_roles=PIPELINE_ROLES)
        self.assertEqual(pending.exception.kind, "NOT_YET_PUBLISHED")
        self.assertEqual(pending.exception.identity, {"udevmon": [10, "root"], "helper": [20, "helper"]})
        for mode in ("changed-root", "missing-pinned-root", "changed-helper", "duplicate", "bad-hash", "wrong-exe", "missing-helper", "strict"):
            with self.subTest(mode=mode):
                current = dict(processes)
                hashes, allowed = {}, PIPELINE_ROLES
                if mode == "changed-root":
                    current[10] = ([b"/usr/bin/udevmon"], 1, "other")
                elif mode == "missing-pinned-root":
                    del current[10]
                elif mode == "changed-helper":
                    current[20] = ([b"/usr/bin/helper"], 1, "other")
                elif mode == "duplicate":
                    current[11] = ([b"/usr/bin/udevmon"], 1, "duplicate")
                elif mode == "bad-hash":
                    hashes = {owner["helper"]: "wrong"}
                elif mode == "missing-helper":
                    del current[20]
                elif mode == "strict":
                    allowed = ()
                reader = Mock(return_value="/foreign/executable") if mode == "wrong-exe" else run
                with self.assertRaises(runtime_facts.FactsFailure) as rejected:
                    runtime_facts.process_identities(current, owner, hashes, reader, privilege=(),
                        not_yet_published_roles=allowed, expected=pending.exception.identity)
                self.assertNotIsInstance(rejected.exception, runtime_facts.ProcessesNotPublished)

    def test_successor_construction_polls_only_until_complete_graph(self):
        import runtime_facts
        before, after = InstalledGuestControls().lifecycle_authority()
        ready = {"x11_health": "ready", "analysis_health": "ready", "input_health": "ready",
                 "learning_ready": 1, "learning_pending": 0, "learning_failed": 0,
                 "learning_completed_generation": 2, "learning_generation": 2,
                 "config_pending": 0, "config_result": "ok"}
        for restoration_only, mode in ((restore, mode) for restore in (False, True)
                                      for mode in ("publish", "expiry", "missing-after-complete", "fatal")):
            with self.subTest(restoration_only=restoration_only, mode=mode):
                guest = Guest.__new__(Guest)
                guest.m = {"processes": {role: role for role in after["identity"]}}
                guest.learning_phase = {"old_dead": True, "current": before, "transition_from": before}
                guest.lifecycle_tray_identity = Mock()
                guest.system_observation = Mock(return_value=after)
                partial = {"udevmon": after["identity"]["udevmon"]}
                pending = runtime_facts.ProcessesNotPublished(partial)
                fault = Failure("wrong hash", "ABORTED_SAFETY")
                values = [pending, ({}, after["identity"]), ({}, after["identity"])]
                if mode == "missing-after-complete":
                    values[-1] = pending
                elif mode == "fatal":
                    values = [fault]
                guest.pipeline_admission = Mock(side_effect=values)
                clock = [0.0]
                with patch("installed_guest.checked_run"), patch("installed_guest.public_stats", return_value=ready) as stats, \
                        patch("installed_guest.time.monotonic", side_effect=lambda: clock[0]), \
                        patch("installed_guest.time.sleep", side_effect=lambda _: clock.__setitem__(0, 3 if mode == "expiry" else .02)) as sleep:
                    if mode == "publish":
                        result = guest.lifecycle_start_pipeline(restoration_only=restoration_only)
                        self.assertEqual(result["pipeline"] if restoration_only else result["after"]["identity"], after["identity"])
                        self.assertEqual(stats.call_args.kwargs["_deadline"], 3)
                        self.assertEqual(guest.pipeline_admission.call_args_list[1].kwargs,
                                         {"construction_identity": {**{role: value for role, value in before["identity"].items()
                                             if role not in PIPELINE_ROLES}, **partial}})
                        self.assertEqual(guest.pipeline_admission.call_args_list[-1].kwargs, {})
                    else:
                        with self.assertRaises((Failure, runtime_facts.ProcessesNotPublished)):
                            guest.lifecycle_start_pipeline(restoration_only=restoration_only)
                        guest.lifecycle_tray_identity.assert_not_called()
                        if mode == "fatal":
                            sleep.assert_not_called()
                        if mode in ("fatal", "expiry"):
                            stats.assert_not_called()

    def test_shared_pipeline_validation_preserves_device_pipe_ancestry_and_output(self):
        import runtime_facts
        identity = {role: [pid, str(pid)] for role, pid in zip(("udevmon", "interception", "daemon", "uinput"), (10, 11, 12, 13))}
        evdev = "/dev/input/by-id/owned-kbd"
        processes = {pid: ([b"process", evdev.encode()], 10, str(pid)) for pid in (11, 12, 13)}
        links = {(11, "0"): "pipe:[source]", (11, "1"): "pipe:[first]", (11, "9"): evdev,
                 (12, "0"): "pipe:[first]", (12, "1"): "pipe:[second]", (12, "9"): "pipe:[unused]",
                 (13, "0"): "pipe:[second]", (13, "1"): "pipe:[sink]", (13, "9"): "/dev/uinput"}
        def run(argv):
            if argv[0] == "ls":
                return "0 1 9"
            path = Path(argv[-1])
            return links[(int(path.parts[2]), path.name)]
        runtime_facts.pipeline_facts(processes, identity, evdev, run, privilege=())
        for key, replacement in (((11, "9"), "/foreign/input"), ((12, "0"), "pipe:[foreign]"),
                                 ((13, "9"), "/foreign/output")):
            previous = links[key]
            links[key] = replacement
            with self.subTest(key=key), self.assertRaises(runtime_facts.FactsFailure):
                runtime_facts.pipeline_facts(processes, identity, evdev, run, privilege=())
            links[key] = previous


if __name__ == "__main__":
    unittest.main()
