#!/usr/bin/env python3

import os
import pathlib
import shutil
import subprocess
import tempfile
import textwrap
import time
import unittest


REPO = pathlib.Path(__file__).resolve().parents[1]
POSTINST = REPO / "DEBIAN" / "postinst"
PRERM = REPO / "DEBIAN" / "prerm"
POSTRM = REPO / "DEBIAN" / "postrm"


def write_executable(path: pathlib.Path, contents: str) -> None:
    path.write_text(textwrap.dedent(contents).lstrip(), encoding="utf-8")
    path.chmod(0o755)


class MaintainerHarness:
    def __init__(self) -> None:
        self._temporary = tempfile.TemporaryDirectory(prefix="punto-maint-contract-")
        self.root = pathlib.Path(self._temporary.name)
        self.etc = self.root / "etc-punto"
        self.bin = self.root / "bin"
        self.sbin = self.root / "usr-sbin"
        self.etc.mkdir()
        self.etc.chmod(0o755)
        self.bin.mkdir()
        self.sbin.mkdir()
        self.tray = self.root / "punto-tray"
        write_executable(self.tray, "#!/bin/sh\nexit 0\n")
        self.calls = self.etc / "calls"
        self.calls.touch()
        self._install_fixtures()

    def cleanup(self) -> None:
        self._temporary.cleanup()

    def _install_fixtures(self) -> None:
        write_executable(
            self.bin / "getent",
            """
            #!/bin/sh
            [ "$1:$2" = group:punto ] || exit 2
            [ -f /etc/punto/group.exists ] || exit 2
            printf 'punto:x:981:\n'
            """,
        )
        write_executable(
            self.bin / "groupadd",
            """
            #!/bin/sh
            printf 'groupadd %s\n' "$*" >>/etc/punto/calls
            [ ! -f /etc/punto/group.exists ] || exit 9
            : >/etc/punto/group.exists
            """,
        )
        write_executable(
            self.bin / "deb-systemd-helper",
            """
            #!/bin/sh
            printf 'deb-systemd-helper %s\n' "$*" >>/etc/punto/calls
            case "$*" in
                'debian-installed udevmon.service')
                    [ -f /etc/punto/systemd-installed ] ;;
                '--quiet was-enabled udevmon.service')
                    [ -f /etc/punto/systemd-enabled ] ;;
                'enable udevmon.service')
                    : >/etc/punto/systemd-installed
                    : >/etc/punto/systemd-enabled ;;
            esac
            """,
        )
        for name in ("update-rc.d", "invoke-rc.d", "deb-systemd-invoke"):
            write_executable(
                self.bin / name,
                f"""
                #!/bin/sh
                printf '{name} %s\\n' "$*" >>/etc/punto/calls
                [ "${{CONTRACT_SERVICE_MODE:-ok}}" = hang-helper ] && sleep 30
                [ "${{CONTRACT_SERVICE_MODE:-ok}}" = fail ] && exit 6
                exit 0
                """,
            )
        write_executable(
            self.sbin / "policy-rc.d",
            """
            #!/bin/sh
            printf 'policy-rc.d %s\n' "$*" >>/etc/punto/calls
            [ "${CONTRACT_POLICY_MODE:-allow}" = hang ] && sleep 30
            [ "${CONTRACT_POLICY_MODE:-allow}" = deny ] && exit 101
            [ "${CONTRACT_POLICY_MODE:-allow}" = error ] && exit 103
            exit 0
            """,
        )
        write_executable(
            self.bin / "systemctl",
            """
            #!/bin/sh
            printf 'systemctl %s\n' "$*" >>/etc/punto/calls
            case "$*" in
                '--system --no-legend --plain --state=active --type=service list-units user@*.service')
                    printf '%s\n' \
                        'user@1000.service loaded active running User Manager for UID 1000' \
                        'user@1001.service loaded active running User Manager for UID 1001' \
                        'user@01.service loaded active running malformed leading zero' \
                        'user@evil.service loaded active running malformed nonnumeric'
                    exit 0
                    ;;
                '--quiet --no-block --user --machine=1000@ try-restart -- punto-tray.service')
                    [ "${CONTRACT_SERVICE_MODE:-ok}" = hang ] && sleep 30
                    [ "${CONTRACT_SERVICE_MODE:-ok}" = fail ] && exit 6
                    [ -f /etc/punto/tray-1000.active ] && \
                        printf 'restarted 1000\n' >>/etc/punto/calls
                    ;;
                '--quiet --no-block --user --machine=1001@ try-restart -- punto-tray.service')
                    [ "${CONTRACT_SERVICE_MODE:-ok}" = hang ] && sleep 30
                    [ "${CONTRACT_SERVICE_MODE:-ok}" = fail ] && exit 6
                    [ -f /etc/punto/tray-1001.active ] && \
                        printf 'restarted 1001\n' >>/etc/punto/calls
                    ;;
                *) exit 97 ;;
            esac
            exit 0
            """,
        )

    def run(
        self,
        script: pathlib.Path,
        action: str,
        service_mode: str = "ok",
        policy_mode: str = "allow",
        previous_version: str | None = None,
    ) -> subprocess.CompletedProcess[str]:
        command = [
            "bwrap",
            "--ro-bind",
            "/",
            "/",
            "--bind",
            str(self.etc),
            "/etc/punto",
            "--ro-bind",
            str(self.tray),
            "/usr/bin/punto-tray",
            "--bind",
            str(self.sbin),
            "/usr/sbin",
            "--dev",
            "/dev",
            "--proc",
            "/proc",
            "--unshare-user",
            "--uid",
            "0",
            "--gid",
            "0",
            "--unshare-pid",
            "--die-with-parent",
            "--new-session",
            "--clearenv",
            "--setenv",
            "PATH",
            f"{self.bin}:/usr/local/sbin:/usr/sbin:/usr/bin:/sbin:/bin",
            "--setenv",
            "CONTRACT_SERVICE_MODE",
            service_mode,
            "--setenv",
            "CONTRACT_POLICY_MODE",
            policy_mode,
            "/bin/sh",
            str(script),
            action,
        ]
        if previous_version is not None:
            command.append(previous_version)
        return subprocess.run(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=7,
            check=False,
        )


class MaintainerScriptContract(unittest.TestCase):
    def setUp(self) -> None:
        if shutil.which("bwrap") is None:
            self.skipTest("bwrap unavailable")
        self.harness = MaintainerHarness()

    def tearDown(self) -> None:
        if hasattr(self, "harness"):
            self.harness.cleanup()

    def test_package_controls_only_its_owned_user_unit(self) -> None:
        postinst = POSTINST.read_text(encoding="utf-8")
        prerm = PRERM.read_text(encoding="utf-8")
        postrm = POSTRM.read_text(encoding="utf-8")
        for source in (prerm, postrm):
            self.assertNotIn("udevmon", source)
            self.assertNotIn("systemctl", source)
            self.assertNotIn("invoke-rc.d", source)
            self.assertNotIn("sudo", source)
        self.assertNotIn("udevmon", postinst)
        self.assertNotIn("sudo", postinst)
        for source in (postinst, postrm):
            self.assertNotIn("deb-systemd-helper", source)
            self.assertNotIn("update-rc.d", source)
        self.assertIn("deb-systemd-invoke --user daemon-reload", postinst)
        self.assertIn("if [ ! -x /usr/bin/punto-tray ]; then", postinst)
        self.assertIn("deb-systemd-invoke --user stop punto-tray.service", postinst)
        self.assertIn('elif [ -n "${2:-}" ]; then', postinst)
        self.assertIn("update_user_tray stop", postinst)
        self.assertIn("update_user_tray restart", postinst)
        self.assertIn("update_user_tray reload", postinst)
        self.assertNotIn("deb-systemd-invoke --user restart punto-tray.service", postinst)
        self.assertIn("timeout --signal=KILL 3s /bin/sh -c", postinst)
        self.assertNotIn("--kill-after", postinst)
        self.assertRegex(postinst, r"try-restart -- \\\s+punto-tray\.service")
        self.assertIn("--machine=\"${punto_user}@\"", postinst)
        self.assertIn("list-units '\\''user@*.service'\\''", postinst)
        self.assertLess(
            postinst.index("policy-rc.d punto-tray.service restart"),
            postinst.index("deb-systemd-invoke --user daemon-reload"),
        )
        self.assertLess(
            postinst.index("deb-systemd-invoke --user daemon-reload"),
            postinst.index("list-units '\\''user@*.service'\\''"),
        )
        self.assertIn(
            "deb-systemd-invoke --user stop punto-tray.service", prerm
        )
        self.assertIn("remove|deconfigure)", prerm)
        self.assertNotIn("upgrade)", prerm)
        self.assertIn("deb-systemd-invoke --user daemon-reload", postrm)

    def test_runtime_gid_is_atomic_and_owned_by_root(self) -> None:
        source = POSTINST.read_text(encoding="utf-8")
        self.assertIn("mktemp", source)
        self.assertIn("chown root:root", source)
        self.assertIn("chmod 0644", source)
        self.assertIn("mv -fT --", source)
        self.assertIn("getent group punto", source)
        self.assertIn("stat -c '%u:%g:%a' /etc/punto", source)
        self.assertNotIn("%F", source)

    def test_upgrade_preserves_active_and_inactive_user_units(self) -> None:
        (self.harness.etc / "tray-1000.active").touch()
        result = self.harness.run(
            POSTINST,
            "configure",
            previous_version="2.8.7",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.harness.calls.read_text(encoding="utf-8").splitlines()
        self.assertEqual(
            [
                line
                for line in calls
                if line.startswith(("deb-systemd-invoke ", "systemctl "))
            ],
            [
                "deb-systemd-invoke --user daemon-reload",
                "systemctl --system --no-legend --plain --state=active --type=service list-units user@*.service",
                "systemctl --quiet --no-block --user --machine=1000@ try-restart -- punto-tray.service",
                "systemctl --quiet --no-block --user --machine=1001@ try-restart -- punto-tray.service",
            ],
        )
        self.assertTrue((self.harness.etc / "tray-1000.active").is_file())
        self.assertFalse((self.harness.etc / "tray-1001.active").exists())
        self.assertIn("restarted 1000", calls)
        self.assertNotIn("restarted 1001", calls)

    def test_full_configure_service_helpers_are_ordered_and_fail_soft(self) -> None:
        result = self.harness.run(
            POSTINST,
            "configure",
            service_mode="fail",
            previous_version="2.8.7",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.harness.calls.read_text(encoding="utf-8").splitlines()
        self.assertEqual(
            [line for line in calls if line.startswith("systemctl ")],
            [
                "systemctl --system --no-legend --plain --state=active --type=service list-units user@*.service",
                "systemctl --quiet --no-block --user --machine=1000@ try-restart -- punto-tray.service",
                "systemctl --quiet --no-block --user --machine=1001@ try-restart -- punto-tray.service",
            ],
        )
        self.assertFalse(any(line.startswith("restarted ") for line in calls))

    def test_hanging_user_manager_is_bounded_and_fail_soft(self) -> None:
        started = time.monotonic()
        result = self.harness.run(
            POSTINST,
            "configure",
            service_mode="hang",
            previous_version="2.8.7",
        )
        elapsed = time.monotonic() - started
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertLess(elapsed, 4.0)

    def test_hanging_policy_is_inside_hard_total_deadline(self) -> None:
        started = time.monotonic()
        result = self.harness.run(
            POSTINST,
            "configure",
            policy_mode="hang",
            previous_version="2.8.7",
        )
        elapsed = time.monotonic() - started
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertLess(elapsed, 4.0)

    def test_hanging_daemon_reload_is_inside_hard_total_deadline(self) -> None:
        started = time.monotonic()
        result = self.harness.run(
            POSTINST,
            "configure",
            service_mode="hang-helper",
            previous_version="2.8.7",
        )
        elapsed = time.monotonic() - started
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertLess(elapsed, 4.0)

    def test_fresh_install_does_not_touch_preexisting_same_name_unit(self) -> None:
        (self.harness.etc / "tray-1000.active").touch()
        result = self.harness.run(POSTINST, "configure")
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.harness.calls.read_text(encoding="utf-8").splitlines()
        self.assertEqual(
            [
                line
                for line in calls
                if line.startswith(("deb-systemd-invoke ", "systemctl "))
            ],
            ["deb-systemd-invoke --user daemon-reload"],
        )
        self.assertNotIn("restarted 1000", calls)

    def test_policy_denial_skips_all_per_user_restarts(self) -> None:
        (self.harness.etc / "tray-1000.active").touch()
        result = self.harness.run(
            POSTINST,
            "configure",
            policy_mode="deny",
            previous_version="2.8.7",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.harness.calls.read_text(encoding="utf-8").splitlines()
        self.assertIn("policy-rc.d punto-tray.service restart", calls)
        self.assertFalse(any(line.startswith("systemctl ") for line in calls))
        self.assertNotIn("restarted 1000", calls)

    def test_policy_error_warns_and_continues_fail_soft(self) -> None:
        result = self.harness.run(
            POSTINST,
            "configure",
            policy_mode="error",
            previous_version="2.8.7",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            "policy-rc.d returned 103; continuing bounded tray refresh",
            result.stderr,
        )
        calls = self.harness.calls.read_text(encoding="utf-8").splitlines()
        self.assertTrue(any(line.startswith("systemctl ") for line in calls))

    def test_runtime_gid_is_removed_only_on_purge(self) -> None:
        self.assertTrue(POSTRM.is_file())
        source = POSTRM.read_text(encoding="utf-8")
        self.assertIn("purge)", source)
        self.assertIn("/etc/punto/runtime-gid", source)
        self.assertNotIn("runtime-gid", PRERM.read_text(encoding="utf-8"))

    def test_scripts_are_idempotent_case_dispatchers(self) -> None:
        self.assertIn("configure)", POSTINST.read_text(encoding="utf-8"))
        self.assertIn("purge)", POSTRM.read_text(encoding="utf-8"))

    def test_configure_is_idempotent_in_disposable_namespace(self) -> None:
        first = self.harness.run(POSTINST, "configure")
        runtime_gid = self.harness.etc / "runtime-gid"
        before = runtime_gid.stat()
        second = self.harness.run(POSTINST, "configure")
        self.assertEqual((first.returncode, second.returncode), (0, 0))
        self.assertEqual(runtime_gid.read_text(encoding="ascii"), "981\n")
        self.assertEqual(runtime_gid.stat().st_mode & 0o777, 0o644)
        after = runtime_gid.stat()
        self.assertEqual(
            (after.st_ino, after.st_mtime_ns, after.st_ctime_ns),
            (before.st_ino, before.st_mtime_ns, before.st_ctime_ns),
        )
        calls = self.harness.calls.read_text(encoding="utf-8").splitlines()
        self.assertEqual(sum(line.startswith("groupadd ") for line in calls), 1)
        self.assertFalse(any("udevmon" in line for line in calls))
        self.assertEqual(
            [line for line in calls if line.startswith("deb-systemd-invoke ")],
            [
                "deb-systemd-invoke --user daemon-reload",
                "deb-systemd-invoke --user daemon-reload",
            ],
        )

    def test_configure_replaces_runtime_gid_symlink_without_touching_victim(self) -> None:
        runtime_gid = self.harness.etc / "runtime-gid"
        victim = self.harness.etc / "victim"
        victim.write_text("preserve\n", encoding="ascii")
        runtime_gid.symlink_to(victim)

        result = self.harness.run(POSTINST, "configure")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(runtime_gid.is_file())
        self.assertFalse(runtime_gid.is_symlink())
        self.assertEqual(runtime_gid.read_text(encoding="ascii"), "981\n")
        self.assertEqual(victim.read_text(encoding="ascii"), "preserve\n")

    def test_configure_replaces_runtime_gid_fifo_without_blocking(self) -> None:
        runtime_gid = self.harness.etc / "runtime-gid"
        os.mkfifo(runtime_gid, 0o600)

        result = self.harness.run(POSTINST, "configure")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(runtime_gid.is_file())
        self.assertEqual(runtime_gid.read_text(encoding="ascii"), "981\n")

    def test_configure_rejects_writable_policy_directory(self) -> None:
        self.harness.etc.chmod(0o777)

        result = self.harness.run(POSTINST, "configure")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not root-owned and protected", result.stderr)
        self.assertFalse((self.harness.etc / "runtime-gid").exists())

    def test_prerm_is_repeatable_and_has_no_external_effects(self) -> None:
        first = self.harness.run(PRERM, "remove")
        second = self.harness.run(PRERM, "deconfigure")
        upgrade = self.harness.run(PRERM, "upgrade")
        self.assertEqual(
            (first.returncode, second.returncode, upgrade.returncode), (0, 0, 0)
        )
        calls = self.harness.calls.read_text(encoding="utf-8").splitlines()
        self.assertFalse(any("udevmon" in line for line in calls))
        self.assertEqual(
            [line for line in calls if line.startswith("deb-systemd-invoke ")],
            ["deb-systemd-invoke --user stop punto-tray.service"] * 2,
        )

    def test_runtime_gid_survives_remove_and_is_unlinked_on_purge(self) -> None:
        runtime_gid = self.harness.etc / "runtime-gid"
        runtime_gid.write_text("981\n", encoding="ascii")
        self.assertEqual(self.harness.run(POSTRM, "remove").returncode, 0)
        self.assertTrue(runtime_gid.is_file())
        self.assertEqual(self.harness.run(POSTRM, "purge").returncode, 0)
        self.assertFalse(runtime_gid.exists())

        calls = self.harness.calls.read_text(encoding="utf-8")
        self.assertNotIn("deb-systemd-helper", calls)
        self.assertNotIn("update-rc.d", calls)

        victim = self.harness.etc / "victim"
        victim.write_text("preserve\n", encoding="ascii")
        runtime_gid.symlink_to(victim)
        self.assertEqual(self.harness.run(POSTRM, "purge").returncode, 0)
        self.assertEqual(victim.read_text(encoding="ascii"), "preserve\n")
        self.assertFalse(runtime_gid.exists())


if __name__ == "__main__":
    unittest.main()
