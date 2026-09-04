#!/usr/bin/env python3
"""Process contract for fatal logging with a blocked inherited stderr."""

from __future__ import annotations

import errno
import fcntl
import os
import pathlib
import subprocess
import sys
import time


def fill_pipe(write_fd: int) -> None:
    try:
        fcntl.fcntl(write_fd, fcntl.F_SETPIPE_SZ, 4096)
    except OSError:
        pass

    os.set_blocking(write_fd, False)
    payload = b"x" * 4096
    try:
        while True:
            os.write(write_fd, payload)
    except OSError as error:
        if error.errno not in (errno.EAGAIN, errno.EWOULDBLOCK):
            raise
    finally:
        os.set_blocking(write_fd, True)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_main_exception_contract.py DAEMON", file=sys.stderr)
        return 2

    daemon = pathlib.Path(sys.argv[1]).resolve()
    if not daemon.is_file() or not os.access(daemon, os.X_OK):
        print(f"daemon is not executable: {daemon}", file=sys.stderr)
        return 2

    read_fd, write_fd = os.pipe2(os.O_CLOEXEC)
    process: subprocess.Popen[bytes] | None = None
    try:
        fill_pipe(write_fd)
        environment = os.environ.copy()
        environment["PUNTO_LOG_STDERR"] = "1"
        environment["PUNTO_TEST_MAIN_EXCEPTION"] = "1"
        started = time.monotonic()
        process = subprocess.Popen(
            [str(daemon)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=write_fd,
            env=environment,
        )
        os.close(write_fd)
        write_fd = -1
        try:
            return_code = process.wait(timeout=5.5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=1)
            print("fatal path blocked forever on inherited stderr", file=sys.stderr)
            return 1
        elapsed = time.monotonic() - started
        if return_code != 3:
            print(f"fatal path returned {return_code}, expected 3", file=sys.stderr)
            return 1
        if not 2.0 <= elapsed < 5.5:
            print(
                f"blocked-sink watchdog elapsed {elapsed:.3f}s, expected [2.0, 5.5)",
                file=sys.stderr,
            )
            return 1
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=1)
        if write_fd >= 0:
            os.close(write_fd)
        os.close(read_fd)

    print("test_main_exception_contract: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
