#!/usr/bin/env python3
"""Native Qt editors, with the second same-process GTK provider used by Telegram."""
import json
import os
import pathlib
import select
import subprocess
import sys
import unittest

import test_event_loop_gtk_e2e as native


QT_APP = r'''
import json, sys
import gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, GLib
from PyQt5 import QtCore, QtWidgets
Gtk.init([])
gtk = Gtk.Window()
gtk.set_accept_focus(False)
gtk.add(Gtk.Entry())
gtk.show_all()
app = QtWidgets.QApplication(['punto-qt-editor-fixture'])
window = QtWidgets.QWidget()
layout = QtWidgets.QVBoxLayout(window)
editors = [QtWidgets.QLineEdit(), QtWidgets.QTextEdit(), QtWidgets.QPlainTextEdit(), QtWidgets.QLineEdit()]
editors[-1].setEchoMode(QtWidgets.QLineEdit.Password)
for editor in editors:
    layout.addWidget(editor)
window.show()
current = editors[0]
current.setFocus()
def command(_fd):
    global current
    value = json.loads(sys.stdin.readline())
    current = editors[value['editor']]
    current.setFocus()
    current.setText(value.get('text', '')) if hasattr(current, 'setText') else current.setPlainText(value.get('text', ''))
    text = current.text() if hasattr(current, 'text') else current.toPlainText()
    print(json.dumps({'ready': True, 'text': text}), flush=True)
def changed():
    text = current.text() if hasattr(current, 'text') else current.toPlainText()
    print(json.dumps({'text': text}), flush=True)
for editor in editors:
    editor.textChanged.connect(changed)
notifier = QtCore.QSocketNotifier(sys.stdin.fileno(), QtCore.QSocketNotifier.Read)
notifier.activated.connect(command)
print(json.dumps({'xid': int(window.winId())}), flush=True)
app.exec_()
'''


class ProviderFixture:
    """Real bus/PID/UID admission with controlled provider replies at the seam."""
    ROOT = '/org/a11y/atspi/accessible/root'
    EDITOR = '/org/a11y/atspi/accessible/editor'
    XML = '''<node>
      <interface name="org.a11y.atspi.Collection"><method name="GetMatches">
        <arg type="(aiia{ss}iaiiasib)" direction="in"/><arg type="u" direction="in"/>
        <arg type="i" direction="in"/><arg type="b" direction="in"/>
        <arg type="a(so)" direction="out"/></method></interface>
      <interface name="org.a11y.atspi.Accessible">
        <method name="GetInterfaces"><arg type="as" direction="out"/></method>
        <method name="GetChildren"><arg type="a(so)" direction="out"/></method>
        <method name="GetRole"><arg type="u" direction="out"/></method>
        <method name="GetState"><arg type="au" direction="out"/></method>
      </interface>
      <interface name="org.a11y.atspi.Text">
        <property name="CaretOffset" type="i" access="read"/>
        <property name="CharacterCount" type="i" access="read"/>
        <method name="GetNSelections"><arg type="i" direction="out"/></method>
        <method name="GetText"><arg type="i" direction="in"/><arg type="i" direction="in"/>
          <arg type="s" direction="out"/></method>
      </interface></node>'''

    def __init__(self, test, focused=True, role=61, on_role=None, broken=False,
                 collection=True, graph=None):
        self.focused, self.role, self.on_role = focused, role, on_role
        self.broken = broken
        self.collection = collection
        self.state_words = None
        self.text_calls = self.role_calls = self.matches_calls = 0
        Gio, GLib = native.Gio, native.GLib
        session = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        address = session.call_sync('org.a11y.Bus', '/org/a11y/bus', 'org.a11y.Bus',
            'GetAddress', None, GLib.VariantType('(s)'), Gio.DBusCallFlags.NONE,
            300, None).unpack()[0]
        self.bus = Gio.DBusConnection.new_for_address_sync(address,
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
            Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
        test.addCleanup(lambda: None if self.bus.is_closed() else self.bus.close_sync(None))
        self.name = self.bus.get_unique_name()
        self.graph = graph or {self.ROOT: [self.EDITOR], self.EDITOR: []}
        node = Gio.DBusNodeInfo.new_for_xml(self.XML)
        for interface in node.interfaces:
            paths = (self.graph.keys() if interface.name.endswith('.Accessible') else
                     [self.ROOT] if interface.name.endswith('.Collection') else [self.EDITOR])
            for path in paths:
                self.bus.register_object(path, interface, self.call, self.property, None)
        self.bus.call_sync('org.a11y.atspi.Registry', self.ROOT, 'org.a11y.atspi.Socket',
            'Embed', GLib.Variant('((so))', ((self.name, self.ROOT),)),
            GLib.VariantType('((so))'), Gio.DBusCallFlags.NONE, 300, None)

    def property(self, _connection, _sender, _path, _interface, _name):
        self.text_calls += 1
        return native.GLib.Variant('i', 0)

    def call(self, _connection, _sender, path, _interface, method, _parameters, invocation):
        GLib = native.GLib
        if method == 'GetInterfaces':
            value = GLib.Variant('(as)', (['org.a11y.atspi.Accessible'] +
                 (['org.a11y.atspi.Collection'] if self.collection else []),))
        elif method == 'GetChildren':
            value = GLib.Variant('(a(so))', ([(self.name, child) if isinstance(child, str)
                else child for child in self.graph[path]],))
        elif method == 'GetMatches':
            self.matches_calls += 1
            if self.broken:
                invocation.return_dbus_error('org.punto.Fixture.Unverified', 'fixture refusal')
                return
            value = GLib.Variant('(a(so))', ([(self.name, self.EDITOR)] if self.focused else [],))
        elif method == 'GetRole':
            self.role_calls += 1
            if self.on_role:
                self.on_role(self)
            value = GLib.Variant('(u)', (self.role,))
        elif method == 'GetState':
            value = GLib.Variant('(au)', (self.state_words if self.state_words is not None else
                [((1 << 7) | (1 << 12)) if self.focused and path == self.EDITOR else 0, 0],))
        else:
            self.text_calls += 1
            value = GLib.Variant('(i)', (0,)) if method == 'GetNSelections' else GLib.Variant('(s)', ('',))
        invocation.return_value(value)


class QtEditorE2E(native.AccessibleContextGtkE2E):
    def setUp(self):
        super().setUp()
        self.prepare_word_editor()
        native.Gio.bus_get_sync(native.Gio.BusType.SESSION, None).call_sync(
            'org.a11y.Bus', '/org/a11y/bus', 'org.freedesktop.DBus.Properties',
            'Set', native.GLib.Variant('(ssv)', ('org.a11y.Status', 'IsEnabled',
                  native.GLib.Variant('b', True))), native.GLib.VariantType('()'),
            native.Gio.DBusCallFlags.NONE, 300, None)
        address = native.Gio.bus_get_sync(native.Gio.BusType.SESSION, None).call_sync(
            'org.a11y.Bus', '/org/a11y/bus', 'org.a11y.Bus', 'GetAddress', None,
            native.GLib.VariantType('(s)'), native.Gio.DBusCallFlags.NONE,
            300, None).unpack()[0]
        # Qt5 reads the desktop's standard X11 accessibility-bus property.
        subprocess.run(['xprop', '-root', '-f', 'AT_SPI_BUS', '8s', '-set',
                        'AT_SPI_BUS', address], check=True, timeout=3)
        environment = dict(os.environ, QT_LINUX_ACCESSIBILITY_ALWAYS_ON='1')
        self.qt = subprocess.Popen([sys.executable, '-c', QT_APP],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1,
            env=environment)
        self.addCleanup(self.stop_qt)
        os.set_blocking(self.qt.stdout.fileno(), False)
        self.qt_buffer = b''
        self.qt_lines = []
        self.qt_text = ''
        self.pump_until(lambda: any('xid' in line for line in self.read_qt()),
                        'owned Qt window realization')
        xid = next(line['xid'] for line in self.qt_lines if 'xid' in line)
        self.xdo('windowfocus', '--sync', str(xid))
        self.publish_active_window(xid)
        self.window.hide()
        self.pump_for(.05)

    def stop_qt(self):
        if self.qt.poll() is None:
            self.qt.terminate()
        try:
            self.qt.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.qt.kill()
            self.qt.wait(timeout=1)

    def read_qt(self):
        self.assertIsNone(self.qt.poll(), 'owned Qt application exited')
        while select.select([self.qt.stdout], [], [], 0)[0]:
            self.qt_buffer += os.read(self.qt.stdout.fileno(), 65536)
        while b'\n' in self.qt_buffer:
            raw, self.qt_buffer = self.qt_buffer.split(b'\n', 1)
            line = json.loads(raw)
            self.qt_lines.append(line)
            if 'text' in line:
                self.qt_text = line['text']
        return self.qt_lines

    def qt_command(self, editor, text='', select_all=False):
        self.qt_lines.clear()
        self.qt.stdin.write(json.dumps({'editor': editor, 'text': text}) + '\n')
        self.qt.stdin.flush()
        self.pump_until(lambda: any(line.get('ready') for line in self.read_qt()),
                        'owned Qt command completion')
        if select_all:
            self.send_chord((native.KEY_LEFTCTRL,), key=native.LETTER_CODES['a'])
            self.pump_until(lambda: self.selection_text(native.Gdk.SELECTION_PRIMARY) == text,
                            'native Qt keyboard selection owns PRIMARY')

    def assert_qt_text(self, text):
        self.pump_until(lambda: (self.read_qt(), self.qt_text)[1] == text,
                        'native Qt text ' + repr(text))

    def assert_two_providers(self):
        Gio, GLib = native.Gio, native.GLib
        session = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        address = session.call_sync('org.a11y.Bus', '/org/a11y/bus', 'org.a11y.Bus',
            'GetAddress', None, GLib.VariantType('(s)'), Gio.DBusCallFlags.NONE,
            300, None).unpack()[0]
        bus = Gio.DBusConnection.new_for_address_sync(address,
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
            Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
        self.addCleanup(bus.close_sync, None)
        providers = []
        previous = []
        def registered():
            roots = bus.call_sync('org.a11y.atspi.Registry', '/org/a11y/atspi/accessible/root',
                'org.a11y.atspi.Accessible', 'GetChildren', None,
                GLib.VariantType('(a(so))'), Gio.DBusCallFlags.NONE, 300, None).unpack()[0]
            providers[:] = [name for name, _ in roots if bus.call_sync(
                'org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                'GetConnectionUnixProcessID', GLib.Variant('(s)', (name,)),
                GLib.VariantType('(u)'), Gio.DBusCallFlags.NONE, 300, None).unpack()[0]
                == self.qt.pid]
            if providers != previous:
                print('[qt-fixture] providers=' + repr(providers), flush=True)
                for provider in providers:
                    details = bus.call_sync(provider, '/org/a11y/atspi/accessible/root',
                        'org.a11y.atspi.Accessible', 'GetInterfaces', None,
                        GLib.VariantType('(as)'), Gio.DBusCallFlags.NONE, 300, None).unpack()[0]
                    print('[qt-fixture] native interfaces=' + repr(details), flush=True)
                previous[:] = providers
            return len(providers) == 2
        self.pump_until(registered, 'two native toolkit roots with the same actual PID', timeout=10)
        self.assertEqual(len(set(providers)), 2)
        for provider in providers:
            interfaces = bus.call_sync(provider, '/org/a11y/atspi/accessible/root',
                'org.a11y.atspi.Accessible', 'GetInterfaces', None,
                GLib.VariantType('(as)'), Gio.DBusCallFlags.NONE, 300, None).unpack()[0]
            print('[qt-fixture] interfaces=' + repr(interfaces), flush=True)

    def assert_qt_editor(self, editor):
        self.assert_two_providers()
        self.set_desktop_layout(0)
        self.qt_command(editor)
        observed = self.ready_editor(self.qt.pid)
        self.assertEqual(observed[8], self.qt.pid)
        self.harness.send_key(native.KEY_PAUSE)
        self.pump_for(.1)
        self.assert_qt_text('')
        self.harness.type_word('ghbdtn')
        self.assert_qt_text('ghbdtn')
        self.harness.send_key(native.KEY_PAUSE)
        self.assert_qt_text('привет')
        self.assertEqual(self.keyboard_group(), 1)
        self.harness.type_word('f')
        self.assert_qt_text('привета')
        self.set_desktop_layout(0)
        self.qt_command(editor, 'ghbdtn', True)
        self.send_chord((native.KEY_LEFTSHIFT,))
        self.assert_qt_text('привет')
        self.set_desktop_layout(0)
        self.qt_command(editor)
        self.harness.type_word('hello')
        self.assert_qt_text('hello')
        self.send_chord((native.KEY_LEFTCTRL,))
        self.assert_qt_text('HELLO')
        self.qt_command(editor, 'AbC', True)
        self.send_chord((native.KEY_LEFTALT,))
        self.assert_qt_text('aBc')
        self.qt_command(editor, 'привет', True)
        self.send_chord((native.KEY_LEFTCTRL, native.KEY_LEFTALT))
        self.assert_qt_text('privet')
        self.set_desktop_layout(0)
        self.qt_command(editor)
        self.harness.type_word('ghbdtn')
        self.harness.send_key(native.KEY_SPACE)
        self.assert_qt_text('привет ')
        self.assertEqual(self.keyboard_group(), 1)
        self.harness.type_word('f')
        self.assert_qt_text('привет а')
        self.assertEqual(self.selection_text(native.Gdk.SELECTION_CLIPBOARD),
                         'startup clipboard baseline')
        for key in (native.KEY_PAUSE, native.KEY_LEFTCTRL, native.KEY_LEFTSHIFT,
                    native.KEY_LEFTALT):
            self.assertFalse(self.key_is_down(key))

    def test_qt_same_pid_line_edit(self):
        self.assert_qt_editor(0)

    def test_qt_same_pid_text_edit(self):
        self.assert_qt_editor(1)

    def test_qt_same_pid_plain_text_edit(self):
        self.assert_qt_editor(2)

    def test_qt_password_rejected_then_recovery(self):
        self.assert_two_providers()
        self.qt_command(3, 'sensitive fixture')
        self.pump_for(.1)
        self.assertNotEqual(self.observe_editor(pid=self.qt.pid)[0], 0)
        self.qt_command(0)
        self.assertEqual(self.ready_editor(self.qt.pid)[0], 0)

    def observe_fixture(self, child=False):
        helper = self.helper_layout_bridge.bound_helper
        if child:
            return self.accessible_call('ObserveEditorOrigin',
                native.GLib.Variant('(suts)', (helper, os.getpid(),
                    native.GLib.get_monotonic_time() + 300_000, '')),
                '(ustssottuiiiiiss)')
        return self.observe_editor(pid=os.getpid())

    def test_provider_ambiguity_rpc_refusal_and_recovery(self):
        first = ProviderFixture(self)
        second = ProviderFixture(self)
        self.assertNotEqual(self.observe_fixture()[0], 0)
        self.assertEqual(first.text_calls + second.text_calls, 0)
        second.focused = False
        self.assertEqual(self.observe_fixture()[0], 0)
        self.assertGreater(first.text_calls, 0)
        second.broken = True
        self.assertNotEqual(self.observe_fixture()[0], 0)
        second.broken = False
        self.assertEqual(self.observe_fixture()[0], 0)

    def test_password_role_never_reads_text_including_child(self):
        provider = ProviderFixture(self, role=40)
        for child in (True, False):
            self.assertNotEqual(self.observe_fixture(child)[0], 0)
            self.assertEqual(provider.text_calls, 0)
        provider.role = 61
        self.assertEqual(self.observe_fixture()[0], 0)
        self.assertGreater(provider.text_calls, 0)
        provider.role_calls = 0
        def password_at_final(value):
            if value.role_calls == 2:
                value.role = 40
        provider.on_role = password_at_final
        provider.text_calls = 0
        self.assertNotEqual(self.observe_fixture(True)[0], 0)
        self.assertEqual(provider.text_calls, 0)

    def test_provider_roster_changed_during_observation_is_stale(self):
        added = []
        def join(_provider):
            if not added:
                added.append(ProviderFixture(self, focused=False))
        provider = ProviderFixture(self, on_role=join)
        observed = self.observe_fixture(True)
        self.assertEqual(observed[0], 2, 'fresh root must invalidate the in-flight roster')
        provider.on_role = None
        self.assertEqual(self.observe_fixture(True)[0], 0)

    def test_provider_native_walk_depth_limit(self):
        # Total nodes and the original deadline bound depth, not a second cutoff.
        for depth in (17, 18):
            with self.subTest(depth=depth):
                nodes = [ProviderFixture.ROOT] + [
                    f'/org/a11y/atspi/accessible/depth{i}' for i in range(1, depth)] + [
                    ProviderFixture.EDITOR]
                graph = {node: nodes[index + 1:index + 2]
                         for index, node in enumerate(nodes)}
                provider = ProviderFixture(self, collection=False, graph=graph)
                self.assertEqual(self.observe_fixture(True)[0], 0)
                self.assertEqual(provider.text_calls, 0)
                provider.bus.close_sync(None)

    def test_provider_native_walk_bounds_cycles_and_capability_changes(self):
        provider = ProviderFixture(self, collection=False)
        self.assertEqual(self.observe_fixture(True)[0], 0)
        original = dict(provider.graph)
        for children in ([provider.ROOT],
                         [(self.helper_layout_bridge.connection.get_unique_name(), provider.EDITOR)],
                         [provider.EDITOR, provider.EDITOR],
                         [f'/org/a11y/atspi/accessible/overflow{i}' for i in range(128)]):
            with self.subTest(children=children[:2]):
                provider.graph = {provider.ROOT: children, provider.EDITOR: []}
                self.assertEqual(self.observe_fixture(True)[0], 4)
                self.assertEqual(provider.text_calls, 0)
        provider.graph = original
        self.assertEqual(self.observe_fixture(True)[0], 0)
        provider.state_words = [0, 0, 0]
        self.assertEqual(self.observe_fixture(True)[0], 4)
        provider.state_words = None
        provider.role_calls = 0
        def change_capability(value):
            value.collection = True
        provider.on_role = change_capability
        self.assertEqual(self.observe_fixture(True)[0], 2)
        provider.on_role = None
        self.assertEqual(self.observe_fixture(True)[0], 0)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: test_event_loop_qt_e2e.py DRIVER SESSION_BRIDGE')
    native.DRIVER, native.SESSION_BRIDGE = map(pathlib.Path, sys.argv[1:])
    if os.environ.get('PUNTO_EVENT_LOOP_E2E_INNER') != '1':
        available = subprocess.run([sys.executable, '-c', 'from PyQt5 import QtWidgets'],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   timeout=5, check=False).returncode == 0
        if not available:
            print('SKIP: Python PyQt5 is unavailable', file=sys.stderr)
            raise SystemExit(1 if os.environ.get('PUNTO_REQUIRE_EVENT_LOOP_E2E') == '1'
                             else native.SKIP_EXIT)
        raise SystemExit(native.run_in_sandbox(native.DRIVER, native.SESSION_BRIDGE,
                                             pathlib.Path(__file__)))
    native.require_private_network_namespace()
    names = [name for name in unittest.defaultTestLoader.getTestCaseNames(QtEditorE2E)
             if name.startswith(('test_qt_', 'test_provider_', 'test_password_role_'))]
    selected = os.environ.get('PUNTO_EVENT_LOOP_E2E_TEST')
    if selected:
        names = selected.split(',')
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.TestSuite(QtEditorE2E(name) for name in names))
    raise SystemExit(not result.wasSuccessful())
