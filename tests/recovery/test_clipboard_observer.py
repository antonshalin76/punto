"""Dry observer controls, not native X or installed-product acceptance."""
from copy import deepcopy
import sys
import inspect
import textwrap
from pathlib import Path
import unittest
from unittest.mock import MagicMock, Mock, patch
import test_installed_guest
from installed_guest import Collector, Failure, Guest, Lines, verify_state


class ClipboardObserverControls(unittest.TestCase):
    def test_transport_send_and_read_share_original_deadline(self):
        channel = Lines.__new__(Lines)
        now = [100_000_000_000]
        def send(_value, **_bounds):
            now[0] += 6_000_000_000
        def read(timeout=10, **bounds):
            deadline = bounds.get("deadline_ns", now[0] + int(timeout * 1e9))
            self.assertEqual(deadline, 110_000_000_000)
            self.assertEqual(deadline - now[0], 4_000_000_000)
            return {"ok": True}
        channel.send, channel.read = Mock(side_effect=send), Mock(side_effect=read)
        with patch("installed_guest.time.monotonic_ns", side_effect=lambda: now[0]):
            channel.call({})

    def snapshot(self, *, generations=(2, 2), owners=(42, 42), missing=False,
                 malformed=False, catalogue_change=False, clock=None, watch_error=None,
                 native_timestamp=True, timestamp_value=123, timestamp_items=1, timestamp_length=None,
                 cached_catalogue=False, target_missing=False, target_invalid=False,
                 timestamp_missing=False):
        import ctypes as C
        import gtk_target
        atom = lambda name: Mock(**{"name.return_value": name})
        catalogue = [atom(name) for name in ("TARGETS", "TIMESTAMP", "UTF8_STRING", "text/plain")]
        clipboard = Mock()
        clipboard.wait_for_targets.side_effect = [(True, catalogue[:-1] if cached_catalogue else catalogue)] * 3
        fresh = Mock()
        fresh.get_targets.side_effect = [(True, catalogue),
            (True, catalogue[:-1] if catalogue_change else catalogue)]
        if target_invalid:
            fresh.get_targets.side_effect = None
            fresh.get_targets.return_value = (False, None)
        width = C.sizeof(C.c_ulong) if native_timestamp else 4
        timestamp_bytes = timestamp_value.to_bytes(width, sys.byteorder) * timestamp_items
        stamp = Mock(**{"get_data_type.return_value": atom("INTEGER"), "get_format.return_value": 32,
            "get_length.return_value": len(timestamp_bytes) if timestamp_length is None else timestamp_length,
            "get_data.return_value": timestamp_bytes})
        data = Mock(**{"get_data_type.return_value": atom("UTF8_STRING"), "get_format.return_value": 8,
            "get_length.return_value": 1, "get_data.return_value": b"a"})
        if malformed:
            stamp.get_format.return_value = 8
        clipboard.wait_for_contents.side_effect = lambda request: (None if target_missing else fresh) if request.name() == "TARGETS" else (
            (None if timestamp_missing else stamp) if request.name() == "TIMESTAMP" else (
                None if missing and request.name() == "text/plain" else data))
        watch = Mock()
        watch.fence.side_effect = [
            {"owner": owners[0], "publication_generation": generations[0], "timestamp": 123},
            watch_error or {"owner": owners[1], "publication_generation": generations[1], "timestamp": 123}]
        with patch("gtk_target.time.monotonic_ns", side_effect=clock or (lambda: 100)):
            result = gtk_target.independent_clipboard_snapshot(clipboard, watch, 1000)
        self.assertEqual([call.args for call in watch.fence.call_args_list], [(1000,), (1000,)])
        return result, clipboard

    def test_exact_independent_snapshot_uses_all_data_and_no_recipient(self):
        result, reader = self.snapshot()
        self.assertEqual(result["owner"], 42)
        self.assertEqual(result["publication_generation"], 2)
        self.assertEqual(result["representations"], {
            "UTF8_STRING": ["UTF8_STRING", 8, "YQ=="], "text/plain": ["UTF8_STRING", 8, "YQ=="]})
        self.assertFalse(result.get("publication_pending"))
        self.assertEqual(reader.wait_for_contents.call_count, 6)
        reader.wait_for_targets.assert_not_called()

    def test_independent_snapshot_uses_fresh_catalogue_not_gtk_cached_targets(self):
        result, reader = self.snapshot(cached_catalogue=True)
        self.assertEqual(set(result["representations"]), {"UTF8_STRING", "text/plain"})
        reader.wait_for_targets.assert_not_called()

    def test_fresh_targets_absence_requires_positive_publication_but_malformed_is_fatal(self):
        with self.assertRaises(ValueError):
            self.snapshot(target_missing=True)
        result, _ = self.snapshot(target_missing=True, generations=(2, 3))
        self.assertTrue(result["publication_pending"])
        for generations in ((2, 2), (2, 3)):
            with self.subTest(generations=generations), self.assertRaisesRegex(
                    ValueError, "invalid clipboard TARGETS metadata"):
                self.snapshot(target_invalid=True, generations=generations)

    def test_same_owner_missing_data_requires_positive_publication(self):
        result, _ = self.snapshot(generations=(2, 3), missing=True)
        self.assertEqual(result, {"publication_pending": True, "before_generation": 2, "after_generation": 3})
        with self.assertRaises(ValueError):
            self.snapshot(missing=True)

    def test_unavailable_timestamp_preserves_numeric_fences_without_changing_classification(self):
        from gtk_target import ClipboardUnavailable
        expected = ("clipboard_fence before_owner=42 before_generation=2 before_timestamp=123 "
                    "after_owner=42 after_generation=2 after_timestamp=123")
        for absent in ({"timestamp_missing": True}, {"target_missing": True}, {"missing": True}):
            with self.subTest(absent=absent):
                with self.assertRaises(ClipboardUnavailable) as caught:
                    self.snapshot(**absent)
                self.assertEqual(getattr(caught.exception, "__notes__", []), [expected])
                self.assertLessEqual(len(expected), 1024)
            with self.subTest(advanced_absent=absent):
                pending, _ = self.snapshot(**absent, generations=(2, 3))
                self.assertEqual(pending, {"publication_pending": True,
                    "before_generation": 2, "after_generation": 3})
        for generations in ((2, 2), (2, 3)):
            with self.subTest(generations=generations), self.assertRaisesRegex(
                    ValueError, "invalid clipboard TIMESTAMP") as caught:
                self.snapshot(malformed=True, generations=generations)
            self.assertNotIsInstance(caught.exception, ClipboardUnavailable)
            self.assertEqual(getattr(caught.exception, "__notes__", []), [])

    def test_timestamp_is_one_native_gulong_not_four_wire_bytes(self):
        result, _ = self.snapshot(native_timestamp=True)
        self.assertEqual(result["owner"], 42)

    def test_timestamp_rejects_zero_high_bits_multiple_items_and_wrong_length(self):
        import ctypes as C
        options = [{"timestamp_value": 0}, {"timestamp_items": 2}, {"timestamp_length": 3}]
        if C.sizeof(C.c_ulong) > 4:
            options.extend([{"timestamp_value": 1 << 32}, {"native_timestamp": False}])
        for setting in options:
            with self.subTest(setting=setting), self.assertRaises(ValueError):
                self.snapshot(**setting)

    def test_actual_watch_fence_counts_every_native_event_and_caps_history(self):
        import ctypes as C
        from gtk_target import ClipboardPublicationWatch
        watch = ClipboardPublicationWatch.__new__(ClipboardPublicationWatch)
        watch.x = Mock()
        watch.display, watch.window, watch.selection, watch.event_type = 1, 2, 3, 88
        watch.generation, watch.timestamp, watch.owner = 7, 123, 42
        event = watch.Event(type=88, window=2, subtype=0, owner=42, selection=3,
                            selection_timestamp=124)
        watch.x.XGetSelectionOwner.return_value = 42
        watch.x.XNextEvent.side_effect = lambda _display, raw: C.memmove(raw, C.byref(event), C.sizeof(event))
        watch.x.XEventsQueued.side_effect = [2, 1, 0]
        with patch("gtk_target.time.monotonic_ns", return_value=100):
            result = watch.fence(1000)
        self.assertEqual(result, {"owner": 42, "publication_generation": 9, "timestamp": 124})
        watch.x.XSync.assert_called_once_with(1, 0)
        watch.x.XEventsQueued.side_effect = None
        watch.x.XEventsQueued.return_value = 1
        with patch("gtk_target.time.monotonic_ns", return_value=100), self.assertRaisesRegex(ValueError, "incomplete"):
            watch.fence(1000)
        event.send_event = 1
        with patch("gtk_target.time.monotonic_ns", return_value=100), self.assertRaisesRegex(ValueError, "unexpected"):
            watch.fence(1000)

    def test_watch_does_not_mix_post_fence_owner_query_with_old_history(self):
        import ctypes as C
        from gtk_target import ClipboardPublicationWatch
        watch = ClipboardPublicationWatch.__new__(ClipboardPublicationWatch)
        watch.x = Mock()
        watch.display, watch.window, watch.selection, watch.event_type = 1, 2, 3, 88
        watch.generation, watch.timestamp, watch.owner = 7, 123, 42
        pending = []
        publication = watch.Event(type=88, window=2, subtype=0, owner=43,
                                  selection=3, selection_timestamp=124)
        def late_owner_reply(*_args):
            # A publication after the Sync reply accompanies this later reply.
            pending.append(publication)
            return 43
        watch.x.XGetSelectionOwner.side_effect = late_owner_reply
        watch.x.XEventsQueued.side_effect = lambda *_args: len(pending)
        watch.x.XNextEvent.side_effect = lambda _display, raw: C.memmove(
            raw, C.byref(pending.pop(0)), C.sizeof(publication))
        with patch("gtk_target.time.monotonic_ns", return_value=100):
            self.assertEqual(watch.fence(1000),
                {"owner": 42, "publication_generation": 7, "timestamp": 123})
            watch.x.XGetSelectionOwner.assert_not_called()
            pending.append(publication)
            self.assertEqual(watch.fence(1000),
                {"owner": 43, "publication_generation": 8, "timestamp": 124})

    def test_watch_startup_subscribes_before_owner_query_and_drains_that_history(self):
        import ctypes as C
        from gtk_target import ClipboardPublicationWatch
        native, fixes, order, pending = Mock(), Mock(), [], []
        native.XOpenDisplay.return_value = 1
        native.XCreateSimpleWindow.return_value = 2
        native.XInternAtom.return_value = 3
        def extension(_display, event, _error):
            C.cast(event, C.POINTER(C.c_int)).contents.value = 88
            return 1
        fixes.XFixesQueryExtension.side_effect = extension
        fixes.XFixesSelectSelectionInput.side_effect = lambda *_args: order.append("subscribe")
        publication = ClipboardPublicationWatch.Event(type=88, window=2, subtype=0,
            owner=43, selection=3, selection_timestamp=124)
        def initial_owner(*_args):
            order.append("owner")
            pending.append(publication)
            return 42
        native.XGetSelectionOwner.side_effect = initial_owner
        native.XSync.side_effect = lambda *_args: order.append("sync")
        native.XEventsQueued.side_effect = lambda *_args: len(pending)
        native.XNextEvent.side_effect = lambda _display, raw: C.memmove(
            raw, C.byref(pending.pop(0)), C.sizeof(publication))
        with patch("gtk_target.C.CDLL", side_effect=[native, fixes]), \
                patch("gtk_target.time.monotonic_ns", return_value=100):
            watch = ClipboardPublicationWatch()
            self.assertEqual(order, ["subscribe", "owner", "sync"])
            self.assertEqual(watch.fence(1000),
                {"owner": 43, "publication_generation": 1, "timestamp": 124})
        native.XGetSelectionOwner.assert_called_once_with(1, 3)

    def test_watch_events_project_same_owner_aba_and_owner_disappearance(self):
        import ctypes as C
        from gtk_target import ClipboardPublicationWatch
        watch = ClipboardPublicationWatch.__new__(ClipboardPublicationWatch)
        watch.x = Mock()
        watch.x.XGetSelectionOwner.return_value = 42
        watch.display, watch.window, watch.selection, watch.event_type = 1, 2, 3, 88
        watch.generation, watch.timestamp, watch.owner = 7, 123, 42
        pending = []
        watch.x.XEventsQueued.side_effect = lambda *_args: len(pending)
        watch.x.XNextEvent.side_effect = lambda _display, raw: C.memmove(
            raw, C.byref(pending.pop(0)), C.sizeof(watch.Event))
        for generation, (subtype, owner) in enumerate(
                ((0, 42), (0, 43), (0, 42), (1, 0), (2, 0)), 8):
            pending.append(watch.Event(type=88, window=2, subtype=subtype, owner=owner,
                selection=3, selection_timestamp=124))
            with patch("gtk_target.time.monotonic_ns", return_value=100):
                result = watch.fence(1000)
            self.assertEqual(result["owner"], owner)
            self.assertEqual(result["publication_generation"], generation)
        self.assertEqual(watch.generation, 12)
        watch.x.XGetSelectionOwner.assert_not_called()

    def test_aba_and_catalogue_drift_require_complete_history(self):
        result, _ = self.snapshot(generations=(2, 4))
        self.assertTrue(result["publication_pending"])
        for options in ({"owners": (42, 43)}, {"catalogue_change": True}, {"malformed": True},
                        {"generations": (2, 1)},
                        {"watch_error": ValueError("history incomplete")}, {"watch_error": EOFError("lost")}):
            with self.subTest(options=options), self.assertRaises((ValueError, EOFError)):
                self.snapshot(**options)

    def test_original_deadline_is_not_renewed_by_reads(self):
        values = iter([100, 100, 100, 1000])
        with self.assertRaises(TimeoutError):
            self.snapshot(clock=lambda: next(values))

    def test_late_complete_transport_and_settled_receipt_cannot_pass(self):
        channel = Lines.__new__(Lines)
        channel.send, channel.read = Mock(), Mock(return_value={"ok": True})
        with patch("installed_guest.time.monotonic_ns", side_effect=[0, 10_000_000_000]), self.assertRaises(Failure):
            channel.call({})
        before, final = test_installed_guest.InstalledGuestControls().tab_states()
        collector = Collector({}, Mock(), None, None)
        collector.rpc = Mock(return_value=final)
        with patch("installed_guest.time.monotonic", side_effect=[0, 5]), self.assertRaises(Failure):
            collector.wait_state("gtk", "entry", before, "привет", [6, 6], 1,
                expected_focus="gtk_text_view", clipboard_payload_restore=True)

    def test_pending_never_adopts_or_inputs_and_real_reader_error_is_fatal(self):
        before, final = test_installed_guest.InstalledGuestControls().tab_states()
        pending = deepcopy(final)
        pending["clipboard"] = {"publication_pending": True, "before_generation": 2, "after_generation": 3}
        collector = Collector({}, Mock(), None, None)
        collector.baseline = deepcopy(before)
        collector.rpc = Mock(side_effect=[pending, final])
        with patch("installed_guest.time.sleep"):
            observed = collector.wait_state("gtk", "entry", before, "привет", [6, 6], 1,
                expected_focus="gtk_text_view", clipboard_payload_restore=True)
        self.assertIs(observed, final)
        self.assertEqual(collector.baseline, before)
        collector.guard.send.assert_not_called()
        initial = Collector({}, Mock(), None, None)
        with self.assertRaises(Failure):
            initial.authority(pending)
        self.assertIsNone(initial.baseline)
        self.assertEqual(*[call.kwargs["deadline_ns"] for call in collector.rpc.call_args_list])
        collector.rpc.side_effect = Failure("real observer EOF", "INVALID_OBSERVATION")
        with self.assertRaisesRegex(Failure, "real observer EOF"):
            collector.wait_state("gtk", "entry", before, "привет", [6, 6], 1, clipboard_payload_restore=True)

    def test_pending_deadline_and_core_drift_are_fatal_without_adoption(self):
        before, pending = test_installed_guest.InstalledGuestControls().tab_states()
        pending["clipboard"] = {"publication_pending": True, "before_generation": 2, "after_generation": 3}
        for mutation in ("late", "identity", "pressed", "focus", "evidence"):
            observed = deepcopy(pending)
            if mutation == "identity":
                observed["identity"] = {"daemon": [43, "99"]}
            elif mutation == "pressed":
                observed["pressed"] = ["ctrl"]
            elif mutation == "focus":
                observed["state"]["focus_target"] = "foreign"
            elif mutation == "evidence":
                observed["clipboard"]["after_generation"] = 2
            collector = Collector({}, Mock(), None, None)
            collector.rpc = Mock(return_value=observed)
            with patch("installed_guest.time.monotonic", side_effect=[0, 6]), self.assertRaises(Failure):
                collector.wait_state("gtk", "entry", before, "привет", [6, 6], 1,
                    expected_focus="gtk_text_view", clipboard_payload_restore=True)
            self.assertIsNone(collector.baseline)
            collector.guard.send.assert_not_called()

    def test_complete_temporary_publication_waits_read_only_for_exact_restore(self):
        before, restored = test_installed_guest.InstalledGuestControls().tab_states()
        temporary = deepcopy(restored)
        # A complete, independently read publication is not a missing conversion.
        temporary["clipboard"]["bytes_base64"] = "0L/RgNC40LLQtdGC"
        temporary["clipboard"]["representations"]["UTF8_STRING"][2] = "0L/RgNC40LLQtdGC"
        for state in (before, temporary, restored):
            state["clipboard"]["publication_generation"] = 0
        collector = Collector({}, Mock(), None, None)
        collector.baseline = deepcopy(before)
        receipts = iter([temporary, restored])
        def read(*_args, **_kwargs):
            self.assertEqual(collector.baseline, before)
            collector.guard.send.assert_not_called()
            return next(receipts)
        collector.rpc = Mock(side_effect=read)
        with patch("installed_guest.time.monotonic", return_value=0), patch("installed_guest.time.sleep"):
            observed = collector.wait_state("gtk", "entry", before, "привет", [6, 6], 1,
                expected_focus="gtk_text_view", clipboard_payload_restore=True)
        self.assertIs(observed, restored)
        self.assertEqual(collector.baseline, before)
        self.assertEqual([call.kwargs["deadline_ns"] for call in collector.rpc.call_args_list], [5_000_000_000] * 2)

    def test_complete_unrestored_publication_exhaustion_and_core_drift_are_fatal(self):
        before, temporary = test_installed_guest.InstalledGuestControls().tab_states()
        temporary["clipboard"]["representations"]["UTF8_STRING"][2] = "dGVtcA=="
        temporary["clipboard"]["bytes_base64"] = "dGVtcA=="
        for mutation in ("deadline", "identity", "pressed", "focus", "freshness", "layout"):
            observed = deepcopy(temporary)
            if mutation == "identity":
                observed["identity"] = {"daemon": [99, "9"]}
            elif mutation == "pressed":
                observed["pressed"] = ["ctrl"]
            elif mutation == "focus":
                observed["state"]["focus_target"] = "foreign"
            elif mutation == "freshness":
                before["observation_id"], observed["observation_id"] = 9, 9
            elif mutation == "layout":
                observed["group"] = 2
            collector = Collector({}, Mock(), None, None)
            collector.baseline = deepcopy(before)
            collector.rpc = Mock(return_value=observed)
            with patch("installed_guest.time.monotonic", side_effect=[0, 5]), self.assertRaises(Failure):
                collector.wait_state("gtk", "entry", before, "привет", [6, 6], 1,
                    expected_focus="gtk_text_view", clipboard_payload_restore=True)
            self.assertEqual(collector.rpc.call_count, 1)
            self.assertEqual(collector.baseline, before)
            collector.guard.send.assert_not_called()

    def test_unmatched_temporary_snapshot_still_checks_core_before_wait(self):
        before, temporary = test_installed_guest.InstalledGuestControls().tab_states()
        temporary["state"]["text"] = "transition"
        temporary["identity"] = {"daemon": [99, "9"]}
        collector = Collector({}, Mock(), None, None)
        collector.rpc = Mock(side_effect=[temporary, AssertionError("core fault must not wait")])
        with patch("installed_guest.time.monotonic", return_value=0), self.assertRaises(Failure):
            collector.wait_state("gtk", "entry", before, "привет", [6, 6], 1,
                expected_focus="gtk_text_view", clipboard_payload_restore=True)
        self.assertEqual(collector.rpc.call_count, 1)
        collector.guard.send.assert_not_called()

    def test_tab_text_precedes_focus_and_restore_without_early_completion(self):
        before, final = test_installed_guest.InstalledGuestControls().tab_states()
        early, focused = deepcopy(final), deepcopy(final)
        early["state"]["focus_target"] = "gtk_entry"
        for item in (early, focused):
            item["clipboard"]["representations"]["UTF8_STRING"][2] = "dGVtcA=="
            item["clipboard"]["bytes_base64"] = "dGVtcA=="
        collector = Collector({}, Mock(), None, None)
        collector.baseline = deepcopy(before)
        collector.rpc = Mock(side_effect=[early, focused, final])
        with patch("installed_guest.time.monotonic", return_value=0), patch("installed_guest.time.sleep"):
            actual = collector.wait_state("gtk", "entry", before, "привет", [6, 6], 1,
                expected_focus="gtk_text_view", clipboard_payload_restore=True)
        self.assertIs(actual, final)
        self.assertEqual(collector.baseline, before)
        self.assertEqual([call.kwargs["deadline_ns"] for call in collector.rpc.call_args_list], [5_000_000_000] * 3)
        collector.guard.send.assert_not_called()

    def test_requested_following_transition_waits_but_foreign_state_is_fatal(self):
        before, final = test_installed_guest.InstalledGuestControls().tab_states()
        before["state"]["following_text"] = "original-following"
        final["state"]["following_text"] = "requested-following"
        early = deepcopy(final)
        early["state"].update(focus_target="gtk_entry", following_text="original-following")
        collector = Collector({}, Mock(), None, None)
        collector.rpc = Mock(side_effect=[early, final])
        with patch("installed_guest.time.monotonic", return_value=0), patch("installed_guest.time.sleep"):
            actual = collector.wait_state("gtk", "entry", before, "привет", [6, 6], 1,
                expected_focus="gtk_text_view", expected_following="requested-following", clipboard_payload_restore=True)
        self.assertIs(actual, final)
        self.assertEqual(collector.rpc.call_count, 2)
        for field in ("focus", "following", "identity", "keys"):
            foreign = deepcopy(early)
            if field == "focus":
                foreign["state"]["focus_target"] = "foreign"
            elif field == "following":
                foreign["state"]["following_text"] = "foreign"
            elif field == "identity":
                foreign["identity"] = {"daemon": [99, "9"]}
            else:
                foreign["pressed"] = ["tab"]
            collector.rpc = Mock(side_effect=[foreign, AssertionError("foreign state must not wait")])
            with self.subTest(field=field), patch("installed_guest.time.monotonic", return_value=0), self.assertRaises(Failure):
                collector.wait_state("gtk", "entry", before, "привет", [6, 6], 1,
                    expected_focus="gtk_text_view", expected_following="requested-following", clipboard_payload_restore=True)
            self.assertEqual(collector.rpc.call_count, 1)
        collector.guard.send.assert_not_called()

    def test_incomplete_focus_or_following_cannot_pass_at_original_expiry(self):
        before, final = test_installed_guest.InstalledGuestControls().tab_states()
        for field in ("focus", "following"):
            incomplete = deepcopy(final)
            if field == "focus":
                incomplete["state"]["focus_target"] = "gtk_entry"
            collector = Collector({}, Mock(), None, None)
            collector.rpc = Mock(return_value=incomplete)
            with self.subTest(field=field), patch("installed_guest.time.monotonic", side_effect=[0, 5]), self.assertRaises(Failure):
                collector.wait_state("gtk", "entry", before, "привет", [6, 6], 1,
                    expected_focus="gtk_text_view", expected_following="requested-following", clipboard_payload_restore=True)
            self.assertIsNone(collector.baseline)
            collector.guard.send.assert_not_called()

    def test_safety_cleanup_keeps_primary_feature_failure_evidence(self):
        collector = Collector({}, Mock(), None, None)
        primary = Failure("Tab focus not complete", "FAIL_PRODUCT")
        safety = Failure("authority changed during status command", "ABORTED_SAFETY")
        collector.rpc = Mock(side_effect=[{"enabled": True}, {"enabled": False}, safety])
        collector.feature_edit = Mock(side_effect=primary)
        with self.assertRaises(Failure) as raised:
            collector.features()
        self.assertIs(raised.exception, safety)
        self.assertEqual(raised.exception.kind, "ABORTED_SAFETY")
        self.assertIs(raised.exception.__cause__, primary)
        self.assertIn("FAIL_PRODUCT", raised.exception.__notes__[0])
        self.assertIn("Tab focus not complete", raised.exception.__notes__[0])

    def test_mapping_uses_transport_upper_not_browser_clock(self):
        channel = Mock()
        channel.call.return_value = {"host_interval_ns": [100, 120], "guest_monotonic_ns": 70}
        collector = Collector({}, Mock(), channel, MagicMock())
        collector.rpc("chromium", "check")
        collector.rpc("chromium", "check", deadline_ns=1000)
        self.assertEqual(channel.call.call_args.args[0]["deadline_ns"], 950)
        self.assertEqual(channel.call.call_args.kwargs, {"deadline_ns": 1000})

    def test_guest_system_reads_only_independent_pinned_client(self):
        guest = Guest.__new__(Guest)
        roles = {name: f"/owned/{name}" for name in ("udevmon", "interception", "daemon", "uinput")}
        guest.m = {"installed": {}, "package": {"version": "test"}, "processes": roles,
                   "pipeline": {"evdev": "/dev/input/fake"}, "features": True}
        guest.gtk, guest.chrome, guest.clipboard_reader = Mock(), Mock(), Mock()
        guest.gtk.child.pid, guest.chrome.pid, guest.clipboard_reader.child.pid = 5, 6, 7
        guest.surface, guest.deadline_ns, guest.display, guest.x = "gtk", 1000, None, Mock()
        guest.x.XQueryKeymap.return_value, guest.x.XkbGetState.return_value = 1, 0
        guest.x.XKeysymToKeycode.return_value = 1
        guest.x.XGetSelectionOwner.return_value = 42
        guest.active_window = Mock(return_value=None)
        stable = {"owner": 42, "publication_generation": 2, "targets": "TARGETS\nUTF8_STRING\n",
                  "bytes_base64": "YQ==", "representations": {"UTF8_STRING": ["UTF8_STRING", 8, "YQ=="]}}
        guest.clipboard_reader.call.return_value = {"clipboard": stable}
        stats = []
        paths = {pid: path for pid, path in enumerate(roles.values(), 1)}
        for pid in range(1, 8):
            stat = Mock()
            stat.parent.name = str(pid)
            raw = ["S", "0" if pid == 1 else "1"] + ["0"] * 17 + [str(100 + pid)]
            stat.read_text.return_value = f"{pid} (owned) " + " ".join(raw)
            args = [paths.get(pid, "/owned/observer").encode()]
            if pid == 2:
                args.append(b"/dev/input/fake")
            stat.parent.__truediv__ = Mock(return_value=Mock(**{"read_bytes.return_value": b"\0".join(args)}))
            stats.append(stat)
        proc = Mock(**{"glob.return_value": stats})
        def command(argv, **_options):
            self.assertNotEqual(argv[0], "xclip", "Guest.system uses alternate unfenced clipboard client")
            if argv[0] == "dpkg-query":
                return "ii  test"
            if argv[:3] == ["sudo", "-n", "ls"]:
                return "0 1 2"
            path = argv[-1]
            if path.endswith("/exe"):
                return paths[int(path.split("/")[2])]
            links = {"/proc/2/fd/0": "pipe:[0]", "/proc/2/fd/1": "pipe:[1]", "/proc/2/fd/2": "/dev/input/fake",
                     "/proc/3/fd/0": "pipe:[1]", "/proc/3/fd/1": "pipe:[2]",
                     "/proc/4/fd/0": "pipe:[2]", "/proc/4/fd/1": "pipe:[3]", "/proc/4/fd/2": "/dev/uinput"}
            self.assertIn(path, links, "unexpected alternate clipboard reader invocation")
            return links[path]
        with patch("installed_guest.Path", side_effect=lambda path: proc if path == "/proc" else Path(path)), \
                patch("installed_guest.checked_run", side_effect=command):
            observed = guest.system(prepare_pending=True)
        self.assertEqual(observed["clipboard"], stable)
        self.assertEqual(observed["identity"]["clipboard_reader"], [7, "107"])
        guest.gtk.call.assert_not_called()
        guest.clipboard_reader.call.assert_called_once_with({"deadline_ns": 1000}, deadline_ns=1000)

    def test_guard_mutations_are_killed_by_existing_controls(self):
        import gtk_target
        def mutant(function, old, new):
            source = textwrap.dedent(inspect.getsource(function))
            self.assertIn(old, source)
            namespace = dict(function.__globals__)
            exec(compile(source.replace(old, new, 1), "observer-guard-mutant", "exec"), namespace)
            return namespace[function.__name__]
        dropped_publication = mutant(gtk_target.independent_clipboard_snapshot,
            'if after["publication_generation"] > before["publication_generation"]:', 'if False:')
        with patch("gtk_target.independent_clipboard_snapshot", dropped_publication), self.assertRaises(ValueError):
            self.test_same_owner_missing_data_requires_positive_publication()
        dropped_bytes = mutant(verify_state, 'original.get(part) != restored.get(part)', 'False')
        with patch("test_installed_guest.verify_state", dropped_bytes), self.assertRaises(AssertionError):
            test_installed_guest.InstalledGuestControls().test_tab_restore_never_accepts_unknown_representation_or_wrong_field()
        renewed_budget = mutant(Lines.call, 'self.read(deadline_ns=deadline_ns)', 'self.read()')
        with patch.object(Lines, "call", renewed_budget), self.assertRaises(AssertionError):
            self.test_transport_send_and_read_share_original_deadline()
        pending_authority = mutant(Collector.authority,
            'if isinstance(authority["clipboard"], dict) and authority["clipboard"].get("publication_pending"):', 'if False:')
        with patch.object(Collector, "authority", pending_authority), self.assertRaises(AssertionError):
            self.test_pending_never_adopts_or_inputs_and_real_reader_error_is_fatal()


if __name__ == "__main__":
    unittest.main()
