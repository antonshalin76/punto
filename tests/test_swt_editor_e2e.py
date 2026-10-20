#!/usr/bin/env python3
"""Installed SWT Text controls in the existing private native editor harness."""
import os
import pathlib
import shutil
import subprocess
import sys
import unittest

import test_editor_matrix_e2e as matrix

native = matrix.native
SWT_JAR = pathlib.Path('/usr/share/dbeaver-ce/plugins/'
    'org.eclipse.swt.gtk.linux.x86_64_3.132.0.v20251124-0642.jar')
SWT_APP = r'''
import org.eclipse.swt.SWT;
import org.eclipse.swt.layout.FillLayout;
import org.eclipse.swt.widgets.Display;
import org.eclipse.swt.widgets.Shell;
import org.eclipse.swt.widgets.Text;
public class PuntoSwtEditor {
    public static void main(String[] arguments) {
        Display display = new Display();
        Shell shell = new Shell(display);
        shell.setText("Punto SWT owned " + arguments[0]);
        shell.setSize(700, 300);
        shell.setLayout(new FillLayout());
        Text editor = new Text(shell, SWT.BORDER |
            (arguments[0].equals("multi") ? SWT.MULTI : SWT.SINGLE));
        shell.open();
        editor.setFocus();
        while (!shell.isDisposed()) {
            if (!display.readAndDispatch()) display.sleep();
        }
        display.dispose();
    }
}
'''


class SwtEditorE2E(matrix.EditorMatrixE2E):
    def start_swt(self, control):
        compiler, java = shutil.which('javac'), shutil.which('java')
        if not compiler or not java or not SWT_JAR.is_file():
            self.skipTest('installed Java compiler/runtime or pinned DBeaver SWT GTK JAR is unavailable')
        self.assertNotIn('NO_AT_BRIDGE', os.environ,
                         'owned SWT child must not inherit DBeaver launcher accessibility suppression')
        source = self.root / 'PuntoSwtEditor.java'
        source.write_text(SWT_APP, encoding='ascii')
        compiled = subprocess.run([compiler, '-encoding', 'UTF-8', '-classpath', str(SWT_JAR),
            '-d', str(self.root), str(source)], capture_output=True, text=True, timeout=20)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        self.launch([java, '-Duser.home=' + str(self.root),
            '-classpath', str(self.root) + os.pathsep + str(SWT_JAR),
            'PuntoSwtEditor', control], '^Punto SWT owned ' + control + r'\s*$', {'SWT_GTK3': '1'})

    def test_swt_single_line(self):
        self.start_swt('single')
        self.exercise()

    def test_swt_multi_line(self):
        self.start_swt('multi')
        self.exercise()


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: test_swt_editor_e2e.py UNMOCKED_LAYOUT_DRIVER SESSION_BRIDGE')
    if os.environ.get('PUNTO_REAL_LAYOUT_BRIDGE_E2E') != '1':
        raise SystemExit('SWT qualification requires PUNTO_REAL_LAYOUT_BRIDGE_E2E=1')
    native.DRIVER, native.SESSION_BRIDGE = map(pathlib.Path, sys.argv[1:])
    if os.environ.get('PUNTO_EVENT_LOOP_E2E_INNER') != '1':
        raise SystemExit(native.run_in_sandbox(native.DRIVER, native.SESSION_BRIDGE,
                                             pathlib.Path(__file__)))
    native.require_private_network_namespace()
    selected = os.environ.get('PUNTO_EVENT_LOOP_E2E_TEST')
    names = selected.split(',') if selected else ['test_swt_single_line', 'test_swt_multi_line']
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.TestSuite(SwtEditorE2E(name) for name in names))
    raise SystemExit(native.SKIP_EXIT if len(result.skipped) == result.testsRun else
                     not result.wasSuccessful())
