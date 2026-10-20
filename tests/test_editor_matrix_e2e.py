#!/usr/bin/env python3
"""Owned native GTK4, Gecko, Electron and Writer documents in a private desktop."""
import json
import os
import pathlib
import signal
import subprocess
import sys
import time
import unittest

import test_event_loop_gtk_e2e as native

KEY_A = 30

GTK4_APP = r'''
import sys
import gi
gi.require_version('Gtk', '4.0')
gi.require_version('GdkX11', '4.0')
from gi.repository import Gtk, GLib, GdkX11
app = Gtk.Application(application_id='org.punto.NativeMatrix')
def activate(app):
    window = Gtk.ApplicationWindow(application=app, title='Punto GTK4 matrix')
    window.set_default_size(640, 240)
    editor = Gtk.TextView() if '--text-view' in sys.argv else Gtk.Entry()
    window.set_child(editor)
    window.present()
    editor.grab_focus()
app.connect('activate', activate)
app.run([])
'''


class EditorMatrixE2E(native.AccessibleContextGtkE2E):
    def setUp(self):
        super().setUp()
        self.prepare_word_editor()
        self.owned = []
        self.addCleanup(self.stop_owned)
        self.root = pathlib.Path('/tmp/punto-editor-matrix') / self._testMethodName
        self.root.mkdir(parents=True, exist_ok=True)
        session = native.Gio.bus_get_sync(native.Gio.BusType.SESSION, None)
        session.call_sync('org.a11y.Bus', '/org/a11y/bus',
            'org.freedesktop.DBus.Properties', 'Set',
            native.GLib.Variant('(ssv)', ('org.a11y.Status', 'IsEnabled',
                native.GLib.Variant('b', True))), native.GLib.VariantType('()'),
            native.Gio.DBusCallFlags.NONE, 300, None)
        address = session.call_sync('org.a11y.Bus', '/org/a11y/bus',
            'org.a11y.Bus', 'GetAddress', None, native.GLib.VariantType('(s)'),
            native.Gio.DBusCallFlags.NONE, 300, None).unpack()[0]
        self.bus = native.Gio.DBusConnection.new_for_address_sync(address,
            native.Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
            native.Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
        self.addCleanup(self.bus.close_sync, None)

    def stop_owned(self):
        for process in self.owned:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
        deadline = time.monotonic() + 3
        while any(process.poll() is None for process in self.owned) and time.monotonic() < deadline:
            time.sleep(.01)
        for process in self.owned:
            # All descendants share the group's PID namespace and private desktop.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=2)

    def launch(self, arguments, title, environment=None):
        process = subprocess.Popen(arguments, start_new_session=True,
            stdout=None, stderr=None,
            env=dict(os.environ, GTK_IM_MODULE='xim', XMODIFIERS='@im=none',
                     **(environment or {})))
        self.owned.append(process)
        windows = []
        def realized():
            result = subprocess.run(['xdotool', 'search', '--onlyvisible', '--name', title],
                capture_output=True, text=True, timeout=2)
            windows[:] = result.stdout.split()
            return bool(windows)
        self.pump_until(realized, 'owned ' + title + ' realization', timeout=20)
        self.xid = int(windows[-1])
        pid = int(self.xdo('getwindowpid', str(self.xid)))
        self.assertEqual(os.getpgid(pid), process.pid, 'window belongs to the owned process group')
        self.editor_pid = pid
        self.xdo('windowfocus', '--sync', str(self.xid))
        self.publish_active_window(self.xid)

    def text(self):
        observed = self.editor_origin
        count = self.bus.call_sync(observed[4], observed[5], 'org.freedesktop.DBus.Properties',
            'Get', native.GLib.Variant('(ss)', ('org.a11y.atspi.Text', 'CharacterCount')),
            native.GLib.VariantType('(v)'), native.Gio.DBusCallFlags.NONE, 300, None).unpack()[0]
        self.assertGreaterEqual(count, 0)
        self.assertLessEqual(count, 4096, 'only the owned small fixture may be read')
        value = self.bus.call_sync(observed[4], observed[5], 'org.a11y.atspi.Text',
            'GetText', native.GLib.Variant('(ii)', (0, count)),
            native.GLib.VariantType('(s)'), native.Gio.DBusCallFlags.NONE, 300, None).unpack()[0]
        self.assertLessEqual(len(value), 4096, 'only the owned small fixture may be read')
        return value.rstrip('\n')

    def assert_text(self, expected):
        self.last_text = None
        def matches():
            self.last_text = self.text()
            return self.last_text == expected
        try:
            self.pump_until(matches,
                            'native fixture text ' + repr(expected), timeout=4)
        except AssertionError:
            print('[native-matrix-last-text] ' + repr(self.last_text), flush=True)
            for target in ('TARGETS', 'UTF8_STRING', 'text/plain;charset=utf-8'):
                result = subprocess.run(['xclip', '-selection', 'primary', '-o', '-target', target],
                    capture_output=True, text=True, timeout=2)
                print('[native-matrix-primary] ' + json.dumps({'target': target,
                    'returncode': result.returncode, 'value': result.stdout[:4096]}), flush=True)
            raise

    def prove_provider(self):
        providers = []
        def registered():
            roots = self.bus.call_sync('org.a11y.atspi.Registry',
                '/org/a11y/atspi/accessible/root', 'org.a11y.atspi.Accessible',
                'GetChildren', None, native.GLib.VariantType('(a(so))'),
                native.Gio.DBusCallFlags.NONE, 300, None).unpack()[0]
            providers.clear()
            for name, path in roots:
                pid = self.bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
                    'org.freedesktop.DBus', 'GetConnectionUnixProcessID',
                    native.GLib.Variant('(s)', (name,)), native.GLib.VariantType('(u)'),
                    native.Gio.DBusCallFlags.NONE, 300, None).unpack()[0]
                if pid == self.editor_pid:
                    providers.append((name, path))
            return providers
        self.pump_until(registered, 'actual owned application AT-SPI registration', timeout=5)
        for name, path in providers:
            interfaces = self.bus.call_sync(name, path, 'org.a11y.atspi.Accessible',
                'GetInterfaces', None, native.GLib.VariantType('(as)'),
                native.Gio.DBusCallFlags.NONE, 300, None).unpack()[0]
            print('[native-matrix] ' + json.dumps({'test': self._testMethodName,
                'pid': self.editor_pid, 'provider': name, 'interfaces': interfaces}), flush=True)
        observed = self.observe_editor(pid=self.editor_pid)
        if observed[0] != 0:
            for name, path in providers:
                try:
                    rule = ([(1 << 7) | (1 << 12), 0], 1, {}, 1, [0, 0, 0, 0], 1, [], 1, False)
                    focused = self.bus.call_sync(name, path, 'org.a11y.atspi.Collection',
                        'GetMatches', native.GLib.Variant('((aiia{ss}iaiiasib)uib)',
                        (rule, 1, 2, True)), native.GLib.VariantType('(a(so))'),
                        native.Gio.DBusCallFlags.NONE, 300, None).unpack()[0]
                    for owner, object_path in focused:
                        self.assertEqual(owner, name)
                        def call(interface, method, arguments, signature):
                            return self.bus.call_sync(owner, object_path, interface, method,
                                arguments, native.GLib.VariantType(signature),
                                native.Gio.DBusCallFlags.NONE, 300, None).unpack()[0]
                        properties = call('org.freedesktop.DBus.Properties', 'GetAll',
                            native.GLib.Variant('(s)', ('org.a11y.atspi.Text',)), '(a{sv})')
                        details = {'role': call('org.a11y.atspi.Accessible', 'GetRole', None, '(u)'),
                            'interfaces': call('org.a11y.atspi.Accessible', 'GetInterfaces', None, '(as)'),
                            'get_all': properties}
                        for field in ('CaretOffset', 'CharacterCount'):
                            details[field] = call('org.freedesktop.DBus.Properties', 'Get',
                                native.GLib.Variant('(ss)', ('org.a11y.atspi.Text', field)), '(v)')
                        print('[native-matrix-metadata] ' + json.dumps(details), flush=True)
                except native.GLib.Error as error:
                    print('[native-matrix-capability-error] ' + str(error), flush=True)
        self.assertEqual(observed[0], 0, 'native focused editor capability; see provider interfaces')
        self.assertEqual(observed[8], self.editor_pid)
        # Observation polling would cancel the product's active helper request.
        self.editor_origin = observed

    def select_owned(self, expected, backward=False, empty_primary=False):
        if backward:
            self.send_chord((native.KEY_LEFTCTRL,), native.KEY_END)
            events = [(native.EV_KEY, native.KEY_LEFTSHIFT, 1), (native.EV_SYN, native.SYN_REPORT, 0)]
            for _ in expected:
                events.extend(((native.EV_KEY, native.KEY_LEFT, 1), (native.EV_SYN, native.SYN_REPORT, 0),
                               (native.EV_KEY, native.KEY_LEFT, 0), (native.EV_SYN, native.SYN_REPORT, 0)))
            events.extend(((native.EV_KEY, native.KEY_LEFTSHIFT, 0), (native.EV_SYN, native.SYN_REPORT, 0)))
            self.harness.send_events(events)
        else:
            self.send_chord((native.KEY_LEFTCTRL,), KEY_A)
        observed = self.editor_origin
        def exact_bounds():
            count = self.bus.call_sync(observed[4], observed[5], 'org.a11y.atspi.Text',
                'GetNSelections', None, native.GLib.VariantType('(i)'),
                native.Gio.DBusCallFlags.NONE, 300, None).unpack()[0]
            if count != 1:
                return False
            bounds = self.bus.call_sync(observed[4], observed[5], 'org.a11y.atspi.Text',
                'GetSelection', native.GLib.Variant('(i)', (0,)), native.GLib.VariantType('(ii)'),
                native.Gio.DBusCallFlags.NONE, 300, None).unpack()
            if tuple(sorted(bounds)) != (0, len(expected)):
                return False
            return self.text() == expected
        self.pump_until(exact_bounds, 'exact owned native selection bounds')
        self.pump_until(lambda: self.selection_text(native.Gdk.SELECTION_PRIMARY) ==
                        ('' if empty_primary else expected),
                        'empty backwards GTK4 PRIMARY' if empty_primary else 'native selected PRIMARY')
        self.assertNotEqual(self.selection_owner(), 0)

    def clear_owned(self, expected):
        self.select_owned(expected)
        self.harness.send_key(native.KEY_BACKSPACE)
        self.assert_text('')

    def exercise(self, backward_selection=False, empty_primary=False,
                 case_modifiers=(native.KEY_LEFTALT,)):
        self.prove_provider()
        self.assert_text('')
        clipboard = self.selection_text(native.Gdk.SELECTION_CLIPBOARD)
        daemon, helper = self.harness.process.pid, self.session_bridge_process.pid
        daemon_start, helper_start = native.process_start_time(daemon), native.process_start_time(helper)
        def select(expected):
            self.select_owned(expected, backward_selection, empty_primary)
        self.harness.type_word('ghbdtn')
        self.assert_text('ghbdtn')
        self.harness.send_key(native.KEY_PAUSE)
        self.assert_text('привет')
        self.assertEqual(self.keyboard_group(), 1)
        self.harness.type_word('f')
        self.assert_text('привета')
        select('привета')
        self.send_chord((native.KEY_LEFTSHIFT,))
        self.assert_text('ghbdtnf')
        self.assertEqual(self.keyboard_group(), 0)
        self.clear_owned('ghbdtnf')
        self.harness.type_word('hello')
        self.assert_text('hello')
        self.send_chord((native.KEY_LEFTCTRL,))
        self.assert_text('HELLO')
        select('HELLO')
        self.send_chord(case_modifiers)
        self.assert_text('hello')
        self.clear_owned('hello')
        self.harness.type_word('ghbdtn')
        self.assert_text('ghbdtn')
        self.harness.send_key(native.KEY_PAUSE)
        self.assert_text('привет')
        select('привет')
        self.send_chord((native.KEY_LEFTCTRL, native.KEY_LEFTALT))
        self.assert_text('privet')
        self.clear_owned('privet')
        self.set_desktop_layout(0)
        self.harness.send_key(native.KEY_PAUSE)
        self.pump_for(.2)
        self.assert_text('')
        self.assertEqual(native.ipc_request(b'SET_STATUS 1\n'), b'OK ENABLED\n')
        self.harness.type_word('ghbdtn')
        self.harness.send_key(native.KEY_SPACE)
        self.assert_text('привет ')
        self.assertEqual(self.keyboard_group(), 1)
        self.harness.type_word('f')
        self.assert_text('привет а')
        self.assertEqual(self.selection_text(native.Gdk.SELECTION_CLIPBOARD), clipboard)
        self.assertEqual(self.harness.process.pid, daemon)
        self.assertEqual(native.process_start_time(daemon), daemon_start)
        self.assertEqual(self.session_bridge_process.pid, helper)
        self.assertEqual(native.process_start_time(helper), helper_start)
        self.assertIsNone(self.harness.process.poll())
        self.assertIsNone(self.session_bridge_process.poll())
        for key in (native.KEY_PAUSE, native.KEY_LEFTCTRL, native.KEY_LEFTSHIFT, native.KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def selection_diagnostic(self, stage):
        owner = self.selection_owner()
        observed = self.editor_origin
        def call(interface, method, arguments, signature):
            return self.bus.call_sync(observed[4], observed[5], interface, method,
                arguments, native.GLib.VariantType(signature),
                native.Gio.DBusCallFlags.NONE, 300, None).unpack()[0]
        properties = call('org.freedesktop.DBus.Properties', 'GetAll',
            native.GLib.Variant('(s)', ('org.a11y.atspi.Text',)), '(a{sv})')
        count = call('org.a11y.atspi.Text', 'GetNSelections', None, '(i)')
        selected = [self.bus.call_sync(observed[4], observed[5], 'org.a11y.atspi.Text',
            'GetSelection', native.GLib.Variant('(i)', (index,)), native.GLib.VariantType('(ii)'),
            native.Gio.DBusCallFlags.NONE, 300, None).unpack() for index in range(min(count, 2))]
        result = {'stage': stage, 'monotonic_us': native.GLib.get_monotonic_time(),
            'owner_xid': owner, 'focus_xid': self.xdo('getwindowfocus'),
            'expected_xid': self.xid, 'properties': properties,
            'selection_count': count, 'selections': selected, 'text': self.text()}
        for target in ('TIMESTAMP', 'TARGETS', 'UTF8_STRING', 'STRING', 'TEXT',
                       'COMPOUND_TEXT', 'text/plain', 'text/plain;charset=utf-8'):
            primary = subprocess.run(['xclip', '-selection', 'primary', '-o', '-target', target],
                capture_output=True, timeout=2)
            result[target] = {'returncode': primary.returncode,
                'data': primary.stdout.hex() if target == 'TIMESTAMP' else
                        primary.stdout.decode('utf-8', errors='replace')[:4096]}
        result['active_window'] = subprocess.run(['xprop', '-root', '_NET_ACTIVE_WINDOW'],
            capture_output=True, text=True, timeout=2).stdout.strip()
        result['owner_properties'] = subprocess.run(['xprop', '-id', str(owner),
            '_NET_WM_PID', 'WM_CLIENT_LEADER'], capture_output=True, text=True,
            timeout=2).stdout.strip() if owner else ''
        print('[native-selection-diagnostic] ' + json.dumps(result), flush=True)
        return result

    def test_matrix_gtk4_selection_diagnostic(self):
        self.launch([sys.executable, '-c', GTK4_APP], '^Punto GTK4 matrix$')
        self.prove_provider()
        self.harness.type_word('ghbdtn')
        self.assert_text('ghbdtn')
        self.harness.send_key(native.KEY_PAUSE)
        deadline = time.monotonic() + 1.2
        captured = False
        while time.monotonic() < deadline:
            self.pump_once()
            observed = self.editor_origin
            selections = self.bus.call_sync(observed[4], observed[5], 'org.a11y.atspi.Text',
                'GetNSelections', None, native.GLib.VariantType('(i)'),
                native.Gio.DBusCallFlags.NONE, 300, None).unpack()[0]
            if selections and not captured:
                captured = True
                self.selection_diagnostic('gtk4-native-selection-active')
            time.sleep(.01)
        self.selection_diagnostic('gtk4-macro-settled')
        self.assert_text('привет')

    def launch_gtk4(self, text_view=False):
        available = subprocess.run([sys.executable, '-c',
            "import gi; gi.require_version('Gtk', '4.0'); from gi.repository import Gtk"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5, check=False)
        self.assertEqual(available.returncode, 0, 'GTK4 typelib is required for the selected native cohort')
        self.launch([sys.executable, '-c', GTK4_APP] + (['--text-view'] if text_view else []),
                    '^Punto GTK4 matrix$')

    def test_matrix_gtk4(self):
        self.launch_gtk4()
        self.exercise(backward_selection=True, empty_primary=True)

    def test_matrix_gtk4_text_view(self):
        self.launch_gtk4(text_view=True)
        self.exercise(backward_selection=True)

    def test_matrix_gnome_text_editor(self):
        binary = pathlib.Path('/usr/bin/gnome-text-editor')
        if not binary.is_file():
            self.skipTest('host GNOME Text Editor is not installed')
        document = self.root / 'punto-gtk4-owned.txt'
        document.write_text('', encoding='ascii')
        self.launch([str(binary), '--new-window', str(document)], 'punto-gtk4-owned',
                    {'GSK_RENDERER': 'cairo'})
        self.xdo('mousemove', '--window', str(self.xid), '150', '180', 'click', '1')
        self.exercise()

    def test_matrix_firefox(self):
        binary = pathlib.Path('/snap/firefox/9036/usr/lib/firefox/firefox')
        if not binary.is_file():
            self.skipTest('the host Firefox revision is not installed')
        profile = self.root / 'firefox'
        profile.mkdir()
        (profile / 'user.js').write_text(
            'user_pref("browser.shell.checkDefaultBrowser", false);\n'
            'user_pref("browser.startup.homepage_override.mstone", "ignore");\n'
            'user_pref("datareporting.policy.dataSubmissionEnabled", false);\n', encoding='ascii')
        page = self.root / 'gecko.html'
        page.write_text('<!doctype html><title>Punto Gecko matrix loading</title>'
            '<textarea id="owned" style="position:fixed;inset:0;width:95vw;height:80vh"></textarea>'
            '<script>addEventListener("load",()=>{const t=document.getElementById("owned");'
            't.focus();t.setSelectionRange(0,0);document.title="Punto Gecko matrix READY";});</script>',
            encoding='ascii')
        self.launch([str(binary), '--no-remote', '--width', '800', '--height', '600',
            '--profile', str(profile), page.as_uri()],
            'Punto Gecko matrix READY', {'MOZ_DISABLE_CONTENT_SANDBOX': '1'})
        self.xdo('mousemove', '--window', str(self.xid), '120', '180', 'click', '1')
        self.exercise(case_modifiers=(native.KEY_RIGHTCTRL, native.KEY_LEFTALT))

    def start_code(self):
        binary = pathlib.Path('/usr/share/code/code')
        if not binary.is_file():
            self.skipTest('host VS Code is not installed')
        document = self.root / 'punto-matrix.txt'
        self.owned_document = document
        document.write_text('', encoding='ascii')
        settings = self.root / 'code-profile/User/settings.json'
        settings.parent.mkdir(parents=True)
        settings.write_text(json.dumps({'editor.accessibilitySupport': 'on',
            'workspace.trust.enabled': False, 'workbench.startupEditor': 'none',
            'telemetry.telemetryLevel': 'off', 'update.mode': 'none'}), encoding='ascii')
        self.launch([str(binary), '--no-sandbox', '--disable-gpu', '--disable-extensions',
            '--skip-welcome', '--skip-release-notes', '--new-window',
            '--user-data-dir=' + str(self.root / 'code-profile'),
            '--extensions-dir=' + str(self.root / 'code-extensions'), str(document)], 'punto-matrix.txt')
        self.xdo('key', 'Escape', 'ctrl+1')
        observations = []
        next_sample = [0.0]
        def stable_focus():
            if time.monotonic() < next_sample[0]:
                return False
            next_sample[0] = time.monotonic() + .1
            current = self.observe_editor(pid=self.editor_pid)
            identity = current[4:7] if current[0] == 0 else None
            observations.append(identity)
            return len(observations) >= 3 and identity is not None and observations[-3:] == [identity] * 3
        self.pump_until(stable_focus, 'settled native VS Code editor identity', timeout=3)

    def test_matrix_code(self):
        self.start_code()
        self.prove_provider()
        clipboard = self.selection_text(native.Gdk.SELECTION_CLIPBOARD)
        daemon = self.harness.process.pid
        helper = self.session_bridge_process.pid
        def saved(expected):
            self.send_chord((native.KEY_LEFTCTRL,), 31)
            self.pump_until(lambda: self.owned_document.read_bytes() == expected.encode(),
                            'owned VS Code model saved as ' + repr(expected))
        def select(expected):
            self.send_chord((native.KEY_LEFTCTRL,), KEY_A)
            self.pump_until(lambda: self.selection_text(native.Gdk.SELECTION_PRIMARY) == expected,
                            'complete native VS Code PRIMARY')
        def clear(expected):
            select(expected)
            self.harness.send_key(native.KEY_BACKSPACE)
            self.assert_text('')
        def completed(expected, description):
            try:
                self.pump_until(lambda: self.harness.stderr.count(
                    b'Word edit dispatch status=2') >= expected, description)
            except AssertionError:
                for line in self.harness.stderr.decode(errors='replace').splitlines():
                    if any(word in line for word in ('[punto]', 'rejection', 'selection_')):
                        print('[native-code-dispatch] ' + line, flush=True)
                raise
        self.harness.type_word('ghbdtn')
        self.assert_text('ghbdtn')
        self.harness.send_key(native.KEY_PAUSE)
        completed(1, 'VS Code current word mutation')
        saved('привет')
        self.assertEqual(self.keyboard_group(), 1)
        self.harness.type_word('f')
        self.assert_text('привета')
        saved('привета')
        select('привета')
        self.send_chord((native.KEY_LEFTSHIFT,))
        completed(2, 'VS Code selected layout mutation')
        saved('ghbdtnf')
        self.assertEqual(self.keyboard_group(), 0)
        clear('ghbdtnf')
        self.harness.type_word('hello')
        self.assert_text('hello')
        self.send_chord((native.KEY_LEFTCTRL,))
        completed(3, 'VS Code current word case mutation')
        saved('HELLO')
        select('HELLO')
        self.send_chord((native.KEY_RIGHTCTRL, native.KEY_LEFTALT))
        completed(4, 'VS Code selected case mutation')
        saved('hello')
        clear('hello')
        self.harness.type_word('ghbdtn')
        self.assert_text('ghbdtn')
        self.harness.send_key(native.KEY_PAUSE)
        completed(5, 'VS Code next RU word mutation')
        saved('привет')
        select('привет')
        self.send_chord((native.KEY_LEFTCTRL, native.KEY_LEFTALT))
        completed(6, 'VS Code selected transliteration mutation')
        saved('privet')
        clear('privet')
        self.set_desktop_layout(0)
        self.harness.send_key(native.KEY_PAUSE)
        self.pump_for(.2)
        self.harness.type_word('ghbdtn')
        self.harness.send_key(native.KEY_SPACE)
        completed(7, 'VS Code AUTO after empty Pause')
        saved('привет ')
        self.assertEqual(self.keyboard_group(), 1)
        self.assertEqual(self.selection_text(native.Gdk.SELECTION_CLIPBOARD), clipboard)
        self.assertEqual(self.harness.process.pid, daemon)
        self.assertEqual(self.session_bridge_process.pid, helper)
        for key in (native.KEY_PAUSE, native.KEY_LEFTCTRL, native.KEY_LEFTSHIFT, native.KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def test_matrix_code_selection_model_oracle(self):
        self.start_code()
        self.prove_provider()
        self.harness.type_word('ghbdtn')
        self.assert_text('ghbdtn')
        self.harness.send_key(native.KEY_PAUSE)
        self.assert_text('привет')
        self.harness.type_word('f')
        self.assert_text('привета')
        self.send_chord((native.KEY_LEFTCTRL,), 31)  # physical Ctrl+S, owned scratch file only
        self.pump_until(lambda: self.owned_document.read_bytes() == 'привета'.encode(),
                        'owned model saved before selection')
        self.send_chord((native.KEY_LEFTCTRL,), KEY_A)
        self.pump_until(lambda: self.selection_text(native.Gdk.SELECTION_PRIMARY) == 'привета',
                        'complete selected model text')
        self.selection_diagnostic('code-before-shift-pause')
        self.send_chord((native.KEY_LEFTSHIFT,))
        self.pump_for(1.3)
        self.selection_diagnostic('code-after-shift-pause')
        self.send_chord((native.KEY_LEFTCTRL,), 31)
        self.pump_for(.3)
        actual = self.owned_document.read_bytes()
        print('[owned-code-model-bytes] ' + actual.hex(), flush=True)
        self.assertEqual(actual, b'ghbdtnf')

    def test_matrix_writer(self):
        binary = pathlib.Path('/usr/bin/libreoffice')
        if not binary.is_file():
            self.skipTest('host LibreOffice is not installed')
        document = self.root / 'punto-writer.txt'
        document.write_text('', encoding='ascii')
        self.launch([str(binary), '-env:UserInstallation=' + (self.root / 'writer-profile').as_uri(),
            '--norestore', '--nologo', '--nodefault', '--nofirststartwizard', '--writer', str(document)],
            'punto-writer', {'SAL_USE_VCLPLUGIN': 'gtk3', 'SAL_ACCESSIBILITY_ENABLED': '1'})
        cohort = []
        for path in pathlib.Path('/proc').glob('[0-9]*/exe'):
            try:
                pid = int(path.parent.name)
                if os.getpgid(pid) == self.owned[-1].pid:
                    maps = (path.parent / 'maps').read_text()
                    cohort.append({'pid': pid, 'exe': os.readlink(path),
                        'gtk3_backend': 'libvclplug_gtk3lo.so' in maps})
            except (ProcessLookupError, FileNotFoundError):
                pass
        print('[writer-owned-cohort] ' + json.dumps({'window_pid': self.editor_pid,
            'processes': cohort}), flush=True)
        self.exercise()


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: test_editor_matrix_e2e.py DRIVER SESSION_BRIDGE')
    native.DRIVER, native.SESSION_BRIDGE = map(pathlib.Path, sys.argv[1:])
    if os.environ.get('PUNTO_EVENT_LOOP_E2E_INNER') != '1':
        raise SystemExit(native.run_in_sandbox(native.DRIVER, native.SESSION_BRIDGE, pathlib.Path(__file__)))
    native.require_private_network_namespace()
    selected = os.environ.get('PUNTO_EVENT_LOOP_E2E_TEST')
    names = selected.split(',') if selected else ['test_matrix_gtk4', 'test_matrix_gtk4_text_view']
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.TestSuite(EditorMatrixE2E(name) for name in names))
    raise SystemExit(not result.wasSuccessful())
