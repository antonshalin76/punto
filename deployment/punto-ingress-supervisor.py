#!/usr/bin/env python3
"""Start one isolated Xorg ingress stream before grabbing its evdev source."""

import argparse
import fcntl
import os
import select
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import time


BOOTSTRAP_MAGIC = 0x50554231
SEALS = (fcntl.F_SEAL_WRITE | fcntl.F_SEAL_SHRINK |
         fcntl.F_SEAL_GROW | fcntl.F_SEAL_SEAL)
CHILDREN = []


def positive_u32(value):
    parsed = int(value, 0)
    if not 0 < parsed <= 0xffffffff:
        raise argparse.ArgumentTypeError("identity must be a nonzero u32")
    return parsed


def absolute_file(value):
    if not os.path.isabs(value):
        raise argparse.ArgumentTypeError("absolute path required")
    metadata = os.stat(value, follow_symlinks=False)
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_mode & 0o022:
        raise argparse.ArgumentTypeError("unsafe regular file")
    return value


def event_device(value):
    if not value.startswith("/dev/input/event") or not value[16:].isdigit():
        raise argparse.ArgumentTypeError("exact /dev/input/eventN required")
    metadata = os.stat(value, follow_symlinks=False)
    if not stat.S_ISCHR(metadata.st_mode):
        raise argparse.ArgumentTypeError("capture source is not a device")
    return value


def stop_children(*_):
    for child in reversed(CHILDREN):
        if child.poll() is None:
            child.terminate()
    for child in reversed(CHILDREN):
        try:
            child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()


def wait_for_socket(path, server, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if server.poll() is not None:
            raise RuntimeError("patched Xorg exited before ingress listener")
        try:
            metadata = os.stat(path, follow_symlinks=False)
        except FileNotFoundError:
            time.sleep(0.02)
            continue
        if (stat.S_ISSOCK(metadata.st_mode) and
                stat.S_IMODE(metadata.st_mode) == 0o600 and
                metadata.st_uid == os.geteuid()):
            return
        raise RuntimeError("unsafe ingress listener")
    raise RuntimeError("patched Xorg ingress listener timeout")


def wait_for_ready(fd, daemon, server, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if daemon.poll() is not None or server.poll() is not None:
            raise RuntimeError("ingress peer exited before producer readiness")
        remaining = max(0, deadline - time.monotonic())
        if select.select([fd], [], [], min(0.05, remaining))[0]:
            if os.read(fd, 1) == b"R":
                return
            raise RuntimeError("producer readiness channel closed")
    raise RuntimeError("producer readiness timeout; capture not started")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xorg", required=True, type=absolute_file)
    parser.add_argument("--punto", required=True, type=absolute_file)
    parser.add_argument("--interception", required=True, type=absolute_file)
    parser.add_argument("--device", required=True, type=event_device)
    parser.add_argument("--display", required=True)
    parser.add_argument("--desktop-uid", required=True, type=positive_u32)
    parser.add_argument("--xauthority", required=True, type=absolute_file)
    parser.add_argument("--runtime-dir", required=True)
    parser.add_argument("--policy-hash", required=True, type=positive_u32)
    parser.add_argument("--build-id", required=True, type=positive_u32)
    args = parser.parse_args()
    if not args.display.startswith(":") or not args.display[1:].isdigit():
        parser.error("numeric X display required")
    runtime = os.stat(args.runtime_dir, follow_symlinks=False)
    if (not stat.S_ISDIR(runtime.st_mode) or runtime.st_uid != os.geteuid() or
            stat.S_IMODE(runtime.st_mode) != 0o700):
        parser.error("runtime directory must be private and owned")
    authority = os.stat(args.xauthority, follow_symlinks=False)
    if (authority.st_uid != args.desktop_uid or
            stat.S_IMODE(authority.st_mode) != 0o600):
        parser.error("Xauthority must belong to the desktop user at mode 0600")

    directory = tempfile.mkdtemp(prefix="punto-ingress-", dir=args.runtime_dir)
    socket_path = os.path.join(directory, "input.sock")
    bootstrap_fd = -1
    ready_read = ready_write = -1
    try:
        bootstrap_fd = os.memfd_create("punto-ingress-bootstrap",
                                       os.MFD_ALLOW_SEALING)
        secret = os.getrandom(32)
        bootstrap = struct.pack("<IIIII32s", BOOTSTRAP_MAGIC,
                                args.policy_hash, args.build_id,
                                os.geteuid(), os.geteuid(), secret)
        if os.write(bootstrap_fd, bootstrap) != len(bootstrap):
            raise RuntimeError("bootstrap write incomplete")
        fcntl.fcntl(bootstrap_fd, fcntl.F_ADD_SEALS, SEALS)
        environment = os.environ.copy()
        environment["PUNTO_INGRESS_BOOTSTRAP_FD"] = str(bootstrap_fd)
        environment["PUNTO_INGRESS_SOCKET"] = socket_path
        environment["PUNTO_INGRESS_TARGET_DISPLAY"] = args.display
        environment["PUNTO_INGRESS_DESKTOP_UID"] = str(args.desktop_uid)
        server = subprocess.Popen(
            [args.xorg, args.display, "-noreset", "-nolisten", "tcp",
             "-auth", args.xauthority],
            env=environment, pass_fds=(bootstrap_fd,),
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
        CHILDREN.append(server)
        wait_for_socket(socket_path, server, 10)

        ready_read, ready_write = os.pipe2(os.O_CLOEXEC)
        environment["PUNTO_INGRESS_READY_FD"] = str(ready_write)
        daemon = subprocess.Popen(
            [args.punto, "--xorg-ingress"], env=environment,
            pass_fds=(bootstrap_fd, ready_write), stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL)
        CHILDREN.append(daemon)
        os.close(ready_write)
        ready_write = -1
        wait_for_ready(ready_read, daemon, server, 10)
        os.close(ready_read)
        ready_read = -1
        os.close(bootstrap_fd)
        bootstrap_fd = -1

        # The evdev grab is the last child: no uinput or stdout fallback.
        capture = subprocess.Popen(
            [args.interception, "-g", args.device],
            stdout=daemon.stdin, stdin=subprocess.DEVNULL)
        CHILDREN.append(capture)
        daemon.stdin.close()
        while all(child.poll() is None for child in CHILDREN):
            time.sleep(0.05)
        return 1
    finally:
        stop_children()
        if ready_read >= 0:
            os.close(ready_read)
        if ready_write >= 0:
            os.close(ready_write)
        if bootstrap_fd >= 0:
            os.close(bootstrap_fd)
        try:
            os.unlink(socket_path)
        except FileNotFoundError:
            pass
        os.rmdir(directory)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, stop_children)
    signal.signal(signal.SIGINT, stop_children)
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print(f"punto ingress refused: {error}", file=sys.stderr)
        sys.exit(1)
