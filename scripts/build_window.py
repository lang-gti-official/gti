#!/usr/bin/env python3
"""Run a full build or a full test suite inside a shared, counted build window.

Every worktree and session of one repository shares the window, which lives
under the Git common directory. A `suite` slot wraps a whole CTest run and a
`build` slot wraps a whole `cmake --build`; focused tests and single-file
compiles do not need a slot. Acquisition waits with a bounded poll and reports
who holds the slots; a slot whose owner has exited is reclaimed automatically.

    python3 scripts/build_window.py --kind suite -- ctest --test-dir build-release -j 12 --test-load 12
    python3 scripts/build_window.py --kind build -- cmake --build build-release -j 8
    python3 scripts/build_window.py --status

The window is advisory coordination between cooperating agents, not a security
boundary. It needs only Python 3 and Git. Owner liveness uses `os.kill(pid, 0)`
on POSIX; other platforms cannot test it, so there a slot is never reclaimed
automatically and `--status` shows which file to delete by hand.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import time
import uuid

DEFAULT_SLOTS = {"suite": 1, "build": 2}
ENV_PREFIX = "GTI_BUILD_WINDOW_"
DEFAULT_POLL_SECONDS = 5.0
MAX_POLL_SECONDS = 60.0
REPORT_SECONDS = 60.0
MUTEX_STALE_SECONDS = 60.0


class WindowError(RuntimeError):
    pass


def now():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def pid_alive(pid):
    """Report whether a holder process still exists.

    POSIX signals the process with 0, which checks existence without touching
    it (a permission error means it exists under another user). Other
    platforms cannot do that without extra dependencies, so they treat every
    recorded owner as alive and leave reclamation to a person.
    """
    if not isinstance(pid, int) or isinstance(pid, bool) or pid <= 0:
        return False
    if os.name != "posix":
        return True
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def environment(name, default, convert, minimum=None):
    raw = os.environ.get(ENV_PREFIX + name)
    if raw is None or not raw.strip():
        return default
    try:
        value = convert(raw.strip())
    except ValueError as error:
        raise WindowError(f"{ENV_PREFIX}{name} must be a {convert.__name__}: {raw!r}") from error
    if minimum is not None and value < minimum:
        raise WindowError(f"{ENV_PREFIX}{name} must be at least {minimum}")
    return value


def directory_for(common=None, repo="."):
    """Resolve the window directory: the environment override, else the Git common directory."""
    override = os.environ.get(ENV_PREFIX + "DIR")
    if override:
        return Path(override).resolve()
    if common is None:
        result = subprocess.run(["git", "-C", str(repo), "rev-parse", "--path-format=absolute",
                                 "--git-common-dir"], capture_output=True, text=True, check=False)
        if result.returncode:
            raise WindowError("not inside a Git repository; pass --dir or set GTI_BUILD_WINDOW_DIR")
        common = result.stdout.strip()
    return Path(common).resolve() / "gti-build-window"


def describe(records):
    if not records:
        return "nobody"
    return "; ".join(
        f"pid {r.get('pid')} since {r.get('time')}: {shlex.join(r.get('command') or ['?'])}"
        for r in records)


def default_log(message):
    print("build-window: " + message, file=sys.stderr, flush=True)


class Window:
    """A counted semaphore per slot kind, stored as one JSON file per holder."""

    def __init__(self, directory, slots=None, poll=None, timeout=None, log=None):
        self.directory = Path(directory)
        self.slots = {kind: environment(kind.upper() + "_SLOTS", count, int, 1)
                      for kind, count in DEFAULT_SLOTS.items()}
        for kind, count in (slots or {}).items():
            if kind not in DEFAULT_SLOTS:
                raise WindowError(f"unknown slot kind {kind!r}; use one of {sorted(DEFAULT_SLOTS)}")
            if count is not None:
                if int(count) < 1:
                    raise WindowError("slot counts must be at least 1")
                self.slots[kind] = int(count)
        self.poll = environment("POLL_SECONDS", DEFAULT_POLL_SECONDS, float, 0.01) if poll is None else float(poll)
        self.poll = min(max(self.poll, 0.01), MAX_POLL_SECONDS)
        self.timeout = environment("TIMEOUT_SECONDS", None, float, 0.0) if timeout is None else float(timeout)
        self.log = log or default_log

    def limit(self, kind):
        if kind not in self.slots:
            raise WindowError(f"unknown slot kind {kind!r}; use one of {sorted(DEFAULT_SLOTS)}")
        return self.slots[kind]

    # -- storage ---------------------------------------------------------

    def holders(self, kind, reclaim=False):
        """Live holder records of one kind; with reclaim, drop records whose owner exited."""
        records = []
        kind_dir = self.directory / kind
        if not kind_dir.is_dir():
            return records
        for path in sorted(kind_dir.glob("*.json")):
            try:
                record = json.loads(path.read_text())
                if not isinstance(record, dict):
                    raise ValueError("not an object")
            except FileNotFoundError:
                continue
            except (OSError, ValueError):
                # Records are written atomically, so an unreadable file is
                # damaged; reclaim it once it is clearly abandoned.
                if reclaim and self._age(path) > MUTEX_STALE_SECONDS:
                    path.unlink(missing_ok=True)
                    self.log(f"removed unreadable {kind} slot record {path.name}")
                continue
            record["file"] = path.name
            record["alive"] = pid_alive(record.get("pid"))
            if not record["alive"]:
                if reclaim:
                    path.unlink(missing_ok=True)
                    self.log(f"reclaimed {kind} slot of exited pid {record.get('pid')}: "
                             f"{shlex.join(record.get('command') or ['?'])}")
                continue
            records.append(record)
        return records

    def status(self):
        return {kind: self.holders(kind) for kind in sorted(self.slots)}

    @staticmethod
    def _age(path):
        try:
            return time.time() - path.stat().st_mtime
        except OSError:
            return 0.0

    @contextmanager
    def _mutex(self):
        """Serialize the count-and-claim step with a short directory lock."""
        path = self.directory / "lock"
        self.directory.mkdir(parents=True, exist_ok=True)
        deadline = time.monotonic() + 2 * MUTEX_STALE_SECONDS
        while True:
            try:
                path.mkdir()
                break
            except FileExistsError:
                self._reclaim_mutex(path)
            if time.monotonic() > deadline:
                raise WindowError(f"could not take the window mutex {path}; inspect its owner.json")
            time.sleep(0.05)
        try:
            (path / "owner.json").write_text(json.dumps({"pid": os.getpid(), "time": now()}))
            yield
        finally:
            shutil.rmtree(path, ignore_errors=True)

    def _reclaim_mutex(self, path):
        owner = path / "owner.json"
        try:
            pid = json.loads(owner.read_text()).get("pid")
        except (OSError, ValueError):
            pid = None
        if pid_alive(pid) and self._age(path) <= MUTEX_STALE_SECONDS:
            return
        stale = path.with_name(f"lock.stale-{uuid.uuid4().hex[:8]}")
        try:
            path.rename(stale)
        except OSError:
            return  # another waiter reclaimed it first
        shutil.rmtree(stale, ignore_errors=True)
        self.log(f"reclaimed the window mutex left by pid {pid}")

    # -- slots -------------------------------------------------------------

    def take(self, kind, command, **extra):
        """Block until a slot of `kind` is free, then claim it and return its record path."""
        limit = self.limit(kind)
        kind_dir = self.directory / kind
        kind_dir.mkdir(parents=True, exist_ok=True)
        started = time.monotonic()
        last_report = None
        last_summary = None
        while True:
            with self._mutex():
                live = self.holders(kind, reclaim=True)
                if len(live) < limit:
                    path = kind_dir / f"{os.getpid()}-{uuid.uuid4().hex[:8]}.json"
                    record = {"pid": os.getpid(), "time": now(), "kind": kind,
                              "command": list(command), "cwd": os.getcwd(), **extra}
                    temporary = path.with_suffix(".tmp")
                    temporary.write_text(json.dumps(record, indent=2) + "\n")
                    temporary.replace(path)
                    if last_report is not None:
                        self.log(f"acquired a {kind} slot after {int(time.monotonic() - started)}s")
                    return path
            waited = time.monotonic() - started
            summary = describe(live)
            if self.timeout is not None and waited >= self.timeout:
                raise WindowError(f"timed out after {int(waited)}s waiting for a {kind} slot "
                                  f"({len(live)}/{limit} held): {summary}")
            if (last_report is None or summary != last_summary
                    or time.monotonic() - last_report >= REPORT_SECONDS):
                self.log(f"waiting for a {kind} slot ({len(live)}/{limit} held): {summary}")
                last_report = time.monotonic()
                last_summary = summary
            delay = self.poll
            if self.timeout is not None:
                delay = min(delay, max(self.timeout - waited, 0.01))
            time.sleep(delay)

    @staticmethod
    def release(path):
        Path(path).unlink(missing_ok=True)

    @contextmanager
    def acquire(self, kind, command, **extra):
        path = self.take(kind, command, **extra)
        try:
            yield path
        finally:
            self.release(path)


def execute(command):
    """Run the wrapped command, forwarding interrupt/terminate so it can stop first."""
    try:
        process = subprocess.Popen(command)
    except FileNotFoundError as error:
        raise WindowError(f"command not found: {command[0]}") from error
    forwarded = []

    def forward(signum, _frame):
        forwarded.append(signum)
        if process.poll() is None:
            try:
                process.send_signal(signal.SIGKILL if len(forwarded) > 2 and hasattr(signal, "SIGKILL")
                                    else signum)
            except (OSError, ValueError):
                pass

    previous = {}
    for name in ("SIGINT", "SIGTERM"):
        number = getattr(signal, name, None)
        if number is not None:
            previous[number] = signal.signal(number, forward)
    try:
        code = process.wait()
    finally:
        for number, handler in previous.items():
            signal.signal(number, handler)
    if code < 0:
        return 128 - code
    return code


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if "--" in argv:
        split = argv.index("--")
        options, command = argv[:split], argv[split + 1:]
    else:
        options, command = argv, []
    parser = argparse.ArgumentParser(
        description="Run a heavy command inside the shared build window.",
        epilog="Put the wrapped command after `--`. Environment: GTI_BUILD_WINDOW_SUITE_SLOTS, "
               "GTI_BUILD_WINDOW_BUILD_SLOTS, GTI_BUILD_WINDOW_POLL_SECONDS, "
               "GTI_BUILD_WINDOW_TIMEOUT_SECONDS, GTI_BUILD_WINDOW_DIR; flags take precedence.")
    parser.add_argument("--kind", choices=sorted(DEFAULT_SLOTS), help="slot kind to hold while the command runs")
    parser.add_argument("--repo", default=".", help="checkout used to find the Git common directory")
    parser.add_argument("--dir", help="window directory (default: <git common dir>/gti-build-window)")
    parser.add_argument("--suite-slots", type=int, help="concurrent suite slots (default 1)")
    parser.add_argument("--build-slots", type=int, help="concurrent build slots (default 2)")
    parser.add_argument("--poll-seconds", type=float, help="sleep between acquisition attempts (default 5, max 60)")
    parser.add_argument("--timeout-seconds", type=float, help="give up waiting after this long (default: wait)")
    parser.add_argument("--status", action="store_true", help="print the current holders as JSON and exit")
    args = parser.parse_args(options)
    try:
        directory = Path(args.dir).resolve() if args.dir else directory_for(repo=args.repo)
        window = Window(directory, slots={"suite": args.suite_slots, "build": args.build_slots},
                        poll=args.poll_seconds, timeout=args.timeout_seconds)
        if args.status:
            if command:
                raise WindowError("--status takes no command")
            print(json.dumps({"directory": str(directory), "slots": window.slots,
                              "holders": window.status()}, indent=2))
            return 0
        if not args.kind:
            raise WindowError("--kind suite|build is required to run a command")
        if not command:
            raise WindowError("put the command to run after `--`")
        with window.acquire(args.kind, command):
            return execute(command)
    except (WindowError, OSError, ValueError) as error:
        print("build-window: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
