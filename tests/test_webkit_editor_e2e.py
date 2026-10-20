#!/usr/bin/env python3
"""Installed WebKitGTK, owned local textarea, existing private native harness."""
import json
import os
import pathlib
import subprocess
import sys
import unittest

import test_editor_matrix_e2e as matrix

native = matrix.native

WEBKIT_APP = r'''
import gi
gi.require_version('Gtk', '3.0')
gi.require_version('WebKit2', '4.1')
from gi.repository import Gtk, WebKit2
Gtk.init([])
window = Gtk.Window(title='Punto WebKit matrix loading')
window.set_default_size(700, 350)
view = WebKit2.WebView()
window.add(view)
def loaded(web, event):
    if event == WebKit2.LoadEvent.FINISHED:
        web.grab_focus()
        web.run_javascript('document.getElementById("owned").focus()', None,
            lambda *unused: window.set_title('Punto WebKit matrix READY'), None)
view.connect('load-changed', loaded)
view.load_html('<!doctype html><textarea id="owned" autofocus '
    'style="position:fixed;inset:0;width:95vw;height:80vh"></textarea>', 'about:blank')
window.show_all()
Gtk.main()
'''


class WebKitEditorE2E(matrix.EditorMatrixE2E):
    def test_webkit_native_textarea(self):
        available = subprocess.run([sys.executable, '-c',
            'import gi; gi.require_version("WebKit2", "4.1"); from gi.repository import WebKit2'],
            capture_output=True, timeout=5)
        if available.returncode != 0:
            self.skipTest('installed Python GI WebKit2 4.1 is unavailable')
        self.launch([sys.executable, '-c', WEBKIT_APP], '^Punto WebKit matrix READY$', {
            'WEBKIT_DISABLE_COMPOSITING_MODE': '1',
            # The outer private PID/mount/network/user sandbox remains mandatory.
            'WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS': '1',
        })
        self.xdo('mousemove', '--window', str(self.xid), '100', '100', 'click', '1')
        roots = self.bus.call_sync('org.a11y.atspi.Registry',
            '/org/a11y/atspi/accessible/root', 'org.a11y.atspi.Accessible', 'GetChildren',
            None, native.GLib.VariantType('(a(so))'), native.Gio.DBusCallFlags.NONE,
            300, None).unpack()[0]
        roster = []
        for name, path in roots:
            pid = self.bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
                'org.freedesktop.DBus', 'GetConnectionUnixProcessID',
                native.GLib.Variant('(s)', (name,)), native.GLib.VariantType('(u)'),
                native.Gio.DBusCallFlags.NONE, 300, None).unpack()[0]
            roster.append({'provider': name, 'pid': pid,
                           'window_pid': self.editor_pid, 'root': path})
        print('[webkit-native-roster] ' + json.dumps(roster), flush=True)
        queue = [(entry['provider'], entry['root']) for entry in roster
                 if entry['pid'] == self.editor_pid]
        visited = set()
        while queue and len(visited) < 20:
            name, path = queue.pop(0)
            if (name, path) in visited:
                continue
            visited.add((name, path))
            role = self.bus.call_sync(name, path, 'org.a11y.atspi.Accessible', 'GetRole',
                None, native.GLib.VariantType('(u)'), native.Gio.DBusCallFlags.NONE,
                300, None).unpack()[0]
            children = self.bus.call_sync(name, path, 'org.a11y.atspi.Accessible', 'GetChildren',
                None, native.GLib.VariantType('(a(so))'), native.Gio.DBusCallFlags.NONE,
                300, None).unpack()[0]
            print('[webkit-native-node] ' + json.dumps({'provider': name, 'path': path,
                'role': role, 'children': children}), flush=True)
            for child in children:
                if child[0] == name:
                    queue.append(child)
                else:
                    pid = self.bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
                        'org.freedesktop.DBus', 'GetConnectionUnixProcessID',
                        native.GLib.Variant('(s)', (child[0],)), native.GLib.VariantType('(u)'),
                        native.Gio.DBusCallFlags.NONE, 300, None).unpack()[0]
                    print('[webkit-embedded-provider] ' + json.dumps({'provider': child[0],
                        'pid': pid, 'window_pid': self.editor_pid}), flush=True)
                    self.skipTest('WebKit native editable tree is exported by a different process; '
                                  'same-PID native editor capability is unavailable')
        self.exercise()


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: test_webkit_editor_e2e.py UNMOCKED_LAYOUT_DRIVER SESSION_BRIDGE')
    if os.environ.get('PUNTO_REAL_LAYOUT_BRIDGE_E2E') != '1':
        raise SystemExit('WebKit qualification requires PUNTO_REAL_LAYOUT_BRIDGE_E2E=1')
    native.DRIVER, native.SESSION_BRIDGE = map(pathlib.Path, sys.argv[1:])
    if os.environ.get('PUNTO_EVENT_LOOP_E2E_INNER') != '1':
        raise SystemExit(native.run_in_sandbox(native.DRIVER, native.SESSION_BRIDGE,
                                             pathlib.Path(__file__)))
    native.require_private_network_namespace()
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.TestSuite([WebKitEditorE2E('test_webkit_native_textarea')]))
    raise SystemExit(native.SKIP_EXIT if len(result.skipped) == result.testsRun else
                     not result.wasSuccessful())
