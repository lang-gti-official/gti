#!/usr/bin/env python3
"""Disposable SSH build/test runs; see docs/architecture/agent-workflow.md.

The remote account and submitted commands are trusted, like a local terminal.
Path checks constrain this helper's extraction and cleanup, not arbitrary code
executed by a build. No credentials, ignored build trees or Git configuration
are copied from the working tree. Committed Git history travels in a bundle;
it must not contain secrets. Heavy files are removed before a run is finished.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
import uuid

SCHEMA = 1
DEFAULT_ROOT = "/private/tmp/gti-remote-runner"
RETENTION = 24 * 60 * 60
MAX_INPUT = 1024 * 1024 * 1024
MAX_RESULTS = 32 * 1024 * 1024
RUN_ID = re.compile(r"run-[0-9a-f]{32}\Z")
TERMINAL = {"passed", "failed", "cancelled", "timed-out", "interrupted"}


class RunnerError(RuntimeError):
    pass


def read_json(path):
    return json.loads(Path(path).read_text())


def write_json(path, value):
    path = Path(path)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def git(repo, *arguments):
    result = subprocess.run(["git", "-C", str(repo), *arguments], capture_output=True)
    if result.returncode:
        raise RunnerError(result.stderr.decode(errors="replace").strip())
    return result.stdout


def local_directory(repo):
    common = git(repo, "rev-parse", "--path-format=absolute", "--git-common-dir").decode().strip()
    return Path(common) / "gti-remote-runner"


def valid_id(value):
    if not RUN_ID.fullmatch(value):
        raise RunnerError("invalid run ID; expected run- followed by 32 lowercase hexadecimal digits")
    return value


def no_symlink(path):
    # Check ancestors as well: a harmless-looking leaf can traverse a symlink.
    for component in (path, *path.parents):
        if component.is_symlink():
            raise RunnerError(f"symlinks are not permitted in managed paths: {component}")


def root_directory(value, create=False):
    path = Path(value)
    if not path.is_absolute() or ".." in path.parts or path.name != "gti-remote-runner":
        raise RunnerError("remote root must be absolute, without '..', and end in gti-remote-runner")
    no_symlink(path)
    if create and not path.exists():
        path.mkdir(parents=True, mode=0o700)
        write_json(path / "owner.json", {"schema": SCHEMA, "uid": os.getuid()})
    try:
        owner = read_json(path / "owner.json")
    except (OSError, ValueError) as error:
        raise RunnerError(f"refusing unmarked remote root: {path}") from error
    if owner != {"schema": SCHEMA, "uid": os.getuid()} or path.stat().st_uid != os.getuid():
        raise RunnerError(f"remote root is not owned by this runner/account: {path}")
    no_symlink(path / "runs")
    no_symlink(path / "window")
    no_symlink(path / "maintenance")
    (path / "runs").mkdir(exist_ok=True)
    return path


def run_directory(root, run_id):
    path = root / "runs" / valid_id(run_id)
    no_symlink(path)
    if read_json(path / "owner.json") != {"schema": SCHEMA, "id": run_id}:
        raise RunnerError(f"refusing unmarked run directory: {path}")
    return path


def alive(pid):
    if not isinstance(pid, int) or pid <= 0:
        return False
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def active(state):
    # A killed worker can leave a native compiler alive. Do not delete its tree.
    return alive(state.get("pid")) or alive(state.get("child_pid"))


def safe_members(archive, maximum):
    members = archive.getmembers()
    total = 0
    seen = set()
    for member in members:
        path = PurePosixPath(member.name)
        if (not member.isfile() or path.is_absolute() or ".." in path.parts
                or not path.parts or member.name in seen or "\\" in member.name):
            raise RunnerError(f"unsafe/duplicate archive entry: {member.name!r}")
        seen.add(member.name)
        total += member.size
        if total > maximum or len(members) > 100000:
            raise RunnerError("archive exceeds runner size/file-count limit")
    return members


def extract(archive_path, destination, maximum):
    with tarfile.open(archive_path) as archive:
        for member in safe_members(archive, maximum):
            path = destination / member.name
            no_symlink(path)
            path.parent.mkdir(parents=True, exist_ok=True)
            with archive.extractfile(member) as source, path.open("xb") as target:
                shutil.copyfileobj(source, target)
            path.chmod(member.mode & 0o777)


def add_bytes(archive, name, contents, mode=0o644):
    entry = tarfile.TarInfo(name)
    entry.size = len(contents)
    entry.mode = mode
    entry.mtime = 0
    archive.addfile(entry, io.BytesIO(contents))


def snapshot(repo, destination):
    """Copy the actual working tree, including nonignored new files and deletions."""
    repo = Path(git(repo, "rev-parse", "--show-toplevel").decode().strip()).resolve()
    head = git(repo, "rev-parse", "HEAD").decode().strip()
    status = git(repo, "status", "--porcelain=v1", "-z")
    if any(line.startswith(b"160000 ") for line in git(repo, "ls-files", "--stage").splitlines()):
        raise RunnerError("submodules are not supported by the disposable snapshot runner")
    names = sorted(set(git(repo, "ls-files", "--cached", "--others", "--exclude-standard", "-z")
                       .decode().rstrip("\0").split("\0")))
    digest = hashlib.sha256()
    stamps = []
    excluded = []
    total = 0
    with tarfile.open(destination / "source.tar", "w") as archive:
        for name in names:
            if not name:
                continue
            parts = PurePosixPath(name).parts
            if any(p == ".git" or p == ".env" or p.startswith(".env.") for p in parts):
                excluded.append(name)
                continue
            path = repo / name
            if not path.exists() and not path.is_symlink():
                continue  # tracked deletion
            if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(repo):
                raise RunnerError(f"snapshot accepts only regular, contained source files: {name}")
            before = path.stat()
            data = path.read_bytes()
            after = path.stat()
            stamp = (before.st_ino, before.st_size, before.st_mtime_ns, before.st_mode)
            if stamp != (after.st_ino, after.st_size, after.st_mtime_ns, after.st_mode):
                raise RunnerError(f"source changed during snapshot: {name}; retry when edits settle")
            stamps.append((path, stamp))
            total += len(data)
            if total > MAX_INPUT // 2 or len(stamps) > 100000:
                raise RunnerError("source snapshot exceeds 512 MiB/100000 files; ignore generated files")
            digest.update(name.encode() + b"\0" + str(before.st_mode & 0o777).encode() + b"\0")
            digest.update(hashlib.sha256(data).digest())
            add_bytes(archive, name, data, before.st_mode & 0o777)
    # History preserves source identity. It must not contain secrets: excluding
    # .env from the working archive does not redact committed Git objects.
    git(repo, "bundle", "create", str(destination / "head.bundle"), "HEAD")
    for path, stamp in stamps:
        current = path.stat()
        if stamp != (current.st_ino, current.st_size, current.st_mtime_ns, current.st_mode):
            raise RunnerError(f"source changed during snapshot: {path}; retry")
    if git(repo, "rev-parse", "HEAD").decode().strip() != head or git(repo, "status", "--porcelain=v1", "-z") != status:
        raise RunnerError("HEAD or working-tree inventory changed during snapshot; retry")
    return {"head": head, "status": status.decode(errors="replace").replace("\0", "\n"),
            "snapshot_sha256": digest.hexdigest(), "files": len(stamps), "bytes": total,
            "excluded": excluded}


def validate_steps(steps):
    if not isinstance(steps, list) or not steps or len(steps) > 50:
        raise RunnerError("steps must be a nonempty JSON array (at most 50 steps)")
    for step in steps:
        if (not isinstance(step, dict) or set(step) != {"name", "kind", "argv", "cwd"}
                or not isinstance(step["name"], str) or not re.fullmatch(r"[a-zA-Z0-9_-]{1,64}", step["name"])
                or step["kind"] not in ("build", "suite") or step["cwd"] not in ("source", "build")
                or not isinstance(step["argv"], list) or not step["argv"]
                or any(not isinstance(a, str) or not a or "\0" in a for a in step["argv"])):
            raise RunnerError("each step needs a safe name, build/suite kind, argv array and source/build cwd")
    if len({step["name"] for step in steps}) != len(steps):
        raise RunnerError("step names must be unique")
    return steps


def default_steps(request):
    # Two build slots share the Mini. Each gets half the CPUs by default; one
    # suite can use the whole host. Explicit job counts allow dedicated runs.
    cpus = os.cpu_count() or 1
    jobs = request.get("jobs") or max(1, cpus // 2)
    test_jobs = request.get("test_jobs") or cpus
    configure = ["cmake", "-S", "{source}", "-B", "{build}", "-G", "Ninja",
                 "-DCMAKE_BUILD_TYPE=" + request["build_type"], "-DBUILD_TESTING=ON"]
    llvm = Path("/opt/homebrew/opt/llvm/lib/cmake/llvm")
    if llvm.is_dir():
        configure.append("-DLLVM_DIR=" + str(llvm))
    configure.extend(request["cmake_args"])
    build = ["cmake", "--build", "{build}", "--parallel", str(jobs)]
    if request["targets"]:
        build.extend(["--target", *request["targets"]])
    steps = [{"name": "configure", "kind": "build", "argv": configure, "cwd": "source"},
             {"name": "build", "kind": "build", "argv": build, "cwd": "source"}]
    if request["full_suite"] or request["ctest_regex"]:
        test = ["ctest", "--test-dir", "{build}", "--output-on-failure", "--no-tests=error",
                "--parallel", str(test_jobs), "--test-load", str(cpus)]
        if request["ctest_regex"]:
            test.extend(["-R", request["ctest_regex"]])
        steps.append({"name": "ctest", "kind": "suite", "argv": test, "cwd": "source"})
    return steps


def collect_results(run, request, state):
    results = run / "results"
    results.mkdir(exist_ok=True)
    write_json(results / "request.json", request)
    write_json(results / "report.json", state)
    remaining = MAX_RESULTS
    truncated = []
    sources = [(p, "logs/" + p.name) for p in sorted((run / "logs").glob("*.log"))]
    if (run / "worker.log").is_file():
        sources.append((run / "worker.log", "logs/worker.log"))
    sources += [(p, "Testing/" + str(p.relative_to(run / "build" / "Testing")))
                for p in sorted((run / "build" / "Testing").rglob("*"))
                if p.is_file() and not p.is_symlink()]
    for source, name in sources:
        target = results / name
        target.parent.mkdir(parents=True, exist_ok=True)
        size = source.stat().st_size
        # Keep the tail of verbose logs (where failures normally appear).
        count = min(size, remaining, 8 * 1024 * 1024)
        with source.open("rb") as stream:
            stream.seek(size - count)
            target.write_bytes(stream.read(count))
        remaining -= count
        if count < size:
            truncated.append(name)
    write_json(results / "truncation.json", {"tail_only": truncated, "limit_bytes": MAX_RESULTS})
    with tarfile.open(run / "results.tar.gz", "w:gz") as archive:
        for path in sorted(results.rglob("*")):
            if path.is_file():
                archive.add(path, arcname=str(path.relative_to(results)), recursive=False)


def cleanup_heavy(run):
    for name in ("source", "build", "input", "logs", "results"):
        path = run / name
        no_symlink(path)
        if path.exists():
            shutil.rmtree(path)
    (run / "upload.tar").unlink(missing_ok=True)


def publish_report(run, state):
    saved = []
    if not state.get("result_error"):
        try:
            with tarfile.open(run / "results.tar.gz", "r:gz") as archive:
                for member in safe_members(archive, MAX_RESULTS + 2 * 1024 * 1024):
                    if member.name != "report.json":
                        saved.append((member, archive.extractfile(member).read()))
        except (OSError, tarfile.TarError, RunnerError) as error:
            state.update(state="failed", exit_code=1, result_error=str(error))
    temporary = run / "results.tmp"
    with tarfile.open(temporary, "w:gz") as archive:
        for member, data in saved:
            add_bytes(archive, member.name, data, member.mode)
        add_bytes(archive, "report.json", json.dumps(state, indent=2).encode())
    temporary.replace(run / "results.tar.gz")
    write_json(run / "state.json", state)


class Cancelled(RunnerError):
    pass


def stop_child(process):
    if process is None:
        return
    # The process can exit while one of its children still holds the group.
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def worker(root_value, run_id):
    from build_window import Window
    root = root_directory(root_value)
    run = run_directory(root, run_id)
    request = read_json(run / "input" / "request.json")
    started = time.time()
    state = {"id": run_id, "state": "running", "pid": os.getpid(), "child_pid": None, "created": started,
             "started": started, "steps": [], "source": request["source"], "exit_code": 1}
    process = None
    previous = {}
    spawning = False
    pending_interrupt = None

    def abort(error):
        nonlocal pending_interrupt
        if spawning:
            pending_interrupt = error
        else:
            raise error

    def spawn(command, **options):
        nonlocal process, spawning
        # A Python signal can fire after fork but before Popen returns. Defer
        # raising until the new process and its group are tracked for cleanup.
        spawning = True
        try:
            process = subprocess.Popen(command, start_new_session=True, **options)
            state["child_pid"] = process.pid
            write_json(run / "state.json", state)
        finally:
            spawning = False
        if pending_interrupt is not None:
            raise pending_interrupt

    def interrupt(_signum, _frame):
        abort(Cancelled("worker interrupted"))

    for number in (signal.SIGINT, signal.SIGTERM):
        previous[number] = signal.signal(number, interrupt)
    # A timer also interrupts a wait for the shared window, not only commands.
    def tick(_signum, _frame):
        if (run / "cancel").exists():
            abort(Cancelled("cancel requested"))
        if time.time() - started >= request["timeout"]:
            abort(TimeoutError("run deadline reached (including window wait)"))

    previous[signal.SIGALRM] = signal.signal(signal.SIGALRM, tick)
    signal.setitimer(signal.ITIMER_REAL, 0.25, 0.25)
    write_json(run / "state.json", state)
    try:
        source = run / "source"
        build = run / "build"
        source.mkdir()
        build.mkdir()
        (run / "logs").mkdir()
        with (run / "logs" / "prepare.log").open("w") as log:
            for command in (["git", "init", "--template=", str(source)],
                            ["git", "-C", str(source), "fetch", str(run / "input" / "head.bundle"), "HEAD"],
                            ["git", "-C", str(source), "update-ref", "HEAD", request["source"]["head"]],
                            ["git", "-C", str(source), "read-tree", "HEAD"]):
                spawn(command, stdout=log, stderr=log)
                if process.wait() != 0:
                    raise RunnerError("could not prepare snapshot repository; see prepare.log")
                process = None
        extract(run / "input" / "source.tar", source, MAX_INPUT // 2)
        steps = validate_steps(request["steps"] or default_steps(request))
        window = Window(root / "window", slots={"build": 2, "suite": 1}, poll=1)
        environment = dict(os.environ, PATH="/opt/homebrew/bin:/usr/local/bin:" + os.environ.get("PATH", "/usr/bin:/bin"),
                           GTI_BUILD_WINDOW_DIR=str(root / "window"),
                           GTI_BUILD_WINDOW_BUILD_SLOTS="2", GTI_BUILD_WINDOW_SUITE_SLOTS="1")
        for step in steps:
            argv = [a.replace("{source}", str(source)).replace("{build}", str(build)) for a in step["argv"]]
            state["phase"] = step["name"]
            state["child_pid"] = None
            write_json(run / "state.json", state)
            step_start = time.time()
            with (run / "logs" / (step["name"] + ".log")).open("w") as log:
                print(shlex.join(argv), file=log, flush=True)
                with window.acquire(step["kind"], argv, run_id=run_id):
                    try:
                        spawn(argv, cwd=run / step["cwd"], env=environment, stdout=log, stderr=log)
                        code = process.wait()
                    finally:
                        # Keep the slot until the command's entire process group
                        # stops, including cancellation/timeout paths.
                        signal.setitimer(signal.ITIMER_REAL, 0)
                        stop_child(process)
                        process = None
                        signal.setitimer(signal.ITIMER_REAL, 0.25, 0.25)
            state["steps"].append({"name": step["name"], "argv": argv, "exit_code": code,
                                   "seconds": time.time() - step_start})
            if code:
                state.update(state="failed", exit_code=code if code > 0 else 128 - code)
                break
            if request["steps"] is None and step["name"] == "configure":
                cache = (build / "CMakeCache.txt").read_text()
                if not re.search(r"^CMAKE_BUILD_TYPE:STRING=(Release|RelWithDebInfo|MinSizeRel)$", cache, re.MULTILINE):
                    raise RunnerError("configured compiler is not optimized; refusing build/test evidence")
        else:
            state.update(state="passed", exit_code=0)
    except Cancelled as error:
        state.update(state="cancelled", exit_code=130, error=str(error))
    except TimeoutError as error:
        state.update(state="timed-out", exit_code=124, error=str(error))
    except Exception as error:
        state.update(state="failed", exit_code=1, error=str(error))
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        # A second cancellation must not interrupt cleanup or strand a child.
        for number in previous:
            signal.signal(number, signal.SIG_IGN)
        stop_child(process)
        state.update(pid=None, child_pid=None, finished=time.time(), phase="cleanup")
        try:
            collect_results(run, request, state)
        except Exception as error:
            state.update(state="failed", exit_code=1, result_error=str(error))
        try:
            cleanup_heavy(run)
        except Exception as error:
            state.update(state="failed", exit_code=1, cleanup_error=str(error))
        state["phase"] = "finished"
        # Publish the final report after cleanup, even if packaging/cleanup
        # failed. A small fallback bundle keeps that diagnostic retrievable.
        try:
            publish_report(run, state)
        finally:
            write_json(run / "state.json", state)
        for number, handler in previous.items():
            signal.signal(number, handler)
    return state["exit_code"]


def prune(root):
    from build_window import Window
    # Status calls from independent agents can discover the same crashed run.
    # Reuse the existing semaphore rather than invent another stale-lock rule.
    with Window(root / "maintenance", slots={"suite": 1}, poll=0.1).acquire("suite", ["remote-runner", "prune"]):
        return prune_locked(root)


def prune_locked(root):
    removed = []
    for candidate in sorted((root / "runs").iterdir()):
        if not RUN_ID.fullmatch(candidate.name):
            continue
        try:
            run = run_directory(root, candidate.name)
            state = read_json(run / "state.json")
            if active(state):
                continue
            age = time.time() - state.get("finished", state.get("created", state.get("started", time.time())))
            if state["state"] not in TERMINAL:
                # Reclaim heavy files after an interrupted worker/reboot. Keep
                # any existing logs for retrieval before the retention deadline.
                if age < 60:
                    continue  # worker launch/upload grace
                request_path = run / "input" / "request.json"
                request = read_json(request_path) if request_path.exists() else {"source": {}, "interrupted_upload": True}
                state.update(state="interrupted", exit_code=1, finished=time.time(), pid=None, child_pid=None)
                collect_results(run, request, state)
                cleanup_heavy(run)
                publish_report(run, state)
            if age >= RETENTION:
                shutil.rmtree(run)
                removed.append(candidate.name)
        except (OSError, ValueError, RunnerError):
            continue  # unknown/unmarked directories are never swept
    return removed


def remote_action(action, root_value, run_id=None):
    root = root_directory(root_value, create=True)
    removed = prune(root)
    if action == "status":
        from build_window import Window
        runs = ([run_directory(root, run_id)] if run_id else sorted((root / "runs").glob("run-*")))
        states = []
        for path in runs:
            states.append(read_json(run_directory(root, path.name) / "state.json"))
        return {"root": str(root), "runs": states, "window": Window(root / "window").status(), "pruned": removed}
    if action == "clean" and not run_id:
        return {"pruned": removed}  # bounded garbage collection, not purge-all
    if action == "submit":
        valid_id(run_id)
        run = root / "runs" / run_id
        run.mkdir(mode=0o700)
        write_json(run / "owner.json", {"schema": SCHEMA, "id": run_id})
        write_json(run / "state.json", {"id": run_id, "state": "uploading", "created": time.time(),
                                        "pid": os.getpid(), "child_pid": None})
        try:
            with (run / "upload.tar").open("wb") as target:
                total = 0
                while True:
                    block = sys.stdin.buffer.read(1024 * 1024)
                    if not block:
                        break
                    total += len(block)
                    if total > MAX_INPUT:
                        raise RunnerError("upload exceeds 1 GiB runner limit")
                    target.write(block)
            incoming = run / "input"
            incoming.mkdir()
            extract(run / "upload.tar", incoming, MAX_INPUT)
            required = {"head.bundle", "source.tar", "request.json", "remote_runner.py", "build_window.py"}
            if {p.name for p in incoming.iterdir()} != required:
                raise RunnerError("upload has unexpected/missing entries")
            request = read_json(incoming / "request.json")
            if request["steps"]:
                validate_steps(request["steps"])
            if not 1 <= request["timeout"] <= 24 * 60 * 60:
                raise RunnerError("timeout must be between 1 second and 24 hours")
            # Retain worker + window code outside heavy input for status/fetch
            # after execution. Detached worker survives a client disconnect.
            for name in ("remote_runner.py", "build_window.py"):
                shutil.copyfile(incoming / name, run / name)
            with (run / "worker.log").open("ab") as log:
                process = subprocess.Popen([sys.executable, str(run / "remote_runner.py"), "_worker",
                                            str(root), run_id], stdin=subprocess.DEVNULL,
                                           stdout=log, stderr=log, start_new_session=True, close_fds=True)
            return {"id": run_id, "pid": process.pid, "root": str(root)}
        except BaseException:
            shutil.rmtree(run)
            raise
    run = run_directory(root, run_id)
    state = read_json(run / "state.json")
    if action == "cancel":
        if state["state"] not in TERMINAL:
            (run / "cancel").touch()
        return state
    if action in ("fetch", "remove"):
        if active(state) or state["state"] not in TERMINAL:
            raise RunnerError("run is still active; cancel it and wait for cleanup first")
        if action == "fetch":
            with (run / "results.tar.gz").open("rb") as stream:
                shutil.copyfileobj(stream, sys.stdout.buffer)
            return None
        if state.get("cleanup_error"):
            raise RunnerError("cleanup failed; inspect the remote directory instead of deleting it blindly")
        shutil.rmtree(run)
        return {"removed": run_id}
    raise RunnerError(f"unknown remote action: {action}")


def source_code():
    return globals().get("RUNNER_SOURCE") or Path(__file__).read_text()


def connection(config, action, run_id=None, input_path=None, output_path=None):
    script = base64.b64encode(source_code().encode()).decode()
    window = base64.b64encode((Path(__file__).parent / "build_window.py").read_bytes()).decode()
    # The status bootstrap reuses the authoritative Window implementation too.
    bootstrap = ("import base64,sys,types; "
                 f"w=types.ModuleType('build_window');exec(base64.b64decode('{window}'),w.__dict__);"
                 "sys.modules['build_window']=w;"
                 f"s=base64.b64decode('{script}').decode();"
                 "exec(compile(s,'<remote_runner>','exec'),{'__name__':'__main__','RUNNER_SOURCE':s})")
    remote = shlex.join([config["python"], "-u", "-c", bootstrap, "_remote", action, config["root"], *([run_id] if run_id else [])])
    command = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", "-o", "ServerAliveInterval=15",
               "-o", "ServerAliveCountMax=3", "--", config["host"], remote]
    with (open(input_path, "rb") if input_path else open(os.devnull, "rb")) as incoming:
        if output_path:
            with open(output_path, "wb") as outgoing:
                result = subprocess.run(command, stdin=incoming, stdout=outgoing)
        else:
            result = subprocess.run(command, stdin=incoming, stdout=subprocess.PIPE)
    if result.returncode:
        raise RunnerError(f"SSH runner action {action} failed ({result.returncode}); run {run_id or '(none)'}. "
                          "Reconnect and use status/fetch; remote cleanup continues independently.")
    return None if output_path else json.loads(result.stdout)


def fetch(config, run_id, destination):
    destination = Path(destination)
    if destination.exists():
        raise RunnerError(f"results destination already exists: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="gti-runner-fetch-", dir=destination.parent) as temporary:
        temporary = Path(temporary)
        connection(config, "fetch", run_id, output_path=temporary / "results.tar.gz")
        output = temporary / "results"
        output.mkdir()
        extract(temporary / "results.tar.gz", output, MAX_RESULTS + 2 * 1024 * 1024)
        report = read_json(output / "report.json")
        if report["id"] != run_id or report["state"] not in TERMINAL:
            raise RunnerError("result identity/state mismatch")
        output.rename(destination)
    # Never discard remote evidence until local extraction/validation succeeds.
    connection(config, "remove", run_id)
    return report


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--repo", default=".", help="source worktree (default: current directory)")
    result.add_argument("--host", help="SSH alias or user@host; saved by configure")
    result.add_argument("--root", help="absolute remote directory ending in gti-remote-runner")
    result.add_argument("--python", help="remote Python 3.9+ executable")
    commands = result.add_subparsers(dest="command", required=True)
    commands.add_parser("configure", help="save shared connection settings in the Git common directory")
    run = commands.add_parser("run", help="snapshot this worktree, build/test remotely and fetch results")
    run.add_argument("--target", action="append", default=[], help="CMake target; repeatable, default all")
    run.add_argument("--ctest-regex", help="CTest -R expression (no matching tests is an error)")
    run.add_argument("--full-suite", action="store_true")
    run.add_argument("--jobs", type=int)
    run.add_argument("--test-jobs", type=int)
    run.add_argument("--build-type", choices=["Release", "RelWithDebInfo", "MinSizeRel"], default="Release")
    run.add_argument("--cmake-arg", action="append", default=[], help="use --cmake-arg=-DNAME=value")
    run.add_argument("--steps-file", help="instead of CMake, execute an exact-argv JSON step plan")
    run.add_argument("--timeout", type=int, default=3 * 60 * 60, help="seconds, including window waits")
    run.add_argument("--detach", action="store_true", help="return run ID immediately; status/fetch later")
    run.add_argument("--results", help="local results directory; default under Git common directory")
    status = commands.add_parser("status", help="show runs and the Mini's shared build window")
    status.add_argument("run_id", nargs="?")
    get = commands.add_parser("fetch", help="retrieve finished results and delete the remote run")
    get.add_argument("run_id")
    get.add_argument("--results")
    cancel = commands.add_parser("cancel", help="request cancellation; worker cleans up before finishing")
    cancel.add_argument("run_id")
    clean = commands.add_parser("clean", help="prune stale runs, or remove one finished run without fetching")
    clean.add_argument("run_id", nargs="?")
    return result


def main(argv=None):
    arguments = list(sys.argv[1:] if argv is None else argv)
    if arguments and arguments[0] == "_worker":
        return worker(*arguments[1:])
    if arguments and arguments[0] == "_remote":
        response = remote_action(*arguments[1:])
        if response is not None:
            print(json.dumps(response, sort_keys=True), flush=True)
        return 0
    args = parser().parse_args(arguments)
    local = local_directory(args.repo)
    path = local / "config.json"
    config = read_json(path) if path.exists() else {}
    config.update({key: value for key, value in {
        "host": args.host or os.environ.get("GTI_REMOTE_HOST"),
        "root": args.root or os.environ.get("GTI_REMOTE_ROOT"), "python": args.python}.items() if value})
    config.setdefault("root", DEFAULT_ROOT)
    config.setdefault("python", "/opt/homebrew/bin/python3")
    if not config.get("host") or config["host"].startswith("-") or any(c.isspace() for c in config["host"]):
        raise RunnerError("configure --host user@host (or set GTI_REMOTE_HOST); SSH keys must already work")
    if args.command == "configure":
        connection(config, "status")  # validate connection and owned root before saving
        local.mkdir(parents=True, exist_ok=True)
        write_json(path, config)
        print(f"Saved {path}; remote window: {config['root']}/window")
        return 0
    if args.command == "run":
        if args.full_suite and args.ctest_regex:
            raise RunnerError("choose --full-suite or --ctest-regex, not both")
        if any(v is not None and v < 1 for v in (args.jobs, args.test_jobs)) or not 1 <= args.timeout <= 86400:
            raise RunnerError("job counts must be positive; timeout must be 1..86400 seconds")
        if any("CMAKE_BUILD_TYPE" in argument or "CMAKE_CONFIGURATION_TYPES" in argument for argument in args.cmake_arg):
            raise RunnerError("choose the optimized configuration with --build-type, not --cmake-arg")
        run_id = "run-" + uuid.uuid4().hex
        steps = validate_steps(read_json(args.steps_file)) if args.steps_file else None
        with tempfile.TemporaryDirectory(prefix="gti-runner-snapshot-") as temporary:
            temporary = Path(temporary)
            identity = snapshot(args.repo, temporary)
            request = {"schema": SCHEMA, "id": run_id, "source": identity, "steps": steps,
                       "targets": args.target, "ctest_regex": args.ctest_regex, "full_suite": args.full_suite,
                       "jobs": args.jobs, "test_jobs": args.test_jobs, "build_type": args.build_type,
                       "cmake_args": args.cmake_arg, "timeout": args.timeout}
            write_json(temporary / "request.json", request)
            packet = temporary / "upload.tar"
            with tarfile.open(packet, "w") as archive:
                for name in ("head.bundle", "source.tar", "request.json"):
                    archive.add(temporary / name, arcname=name, recursive=False)
                add_bytes(archive, "remote_runner.py", source_code().encode())
                add_bytes(archive, "build_window.py", (Path(__file__).parent / "build_window.py").read_bytes())
            print(f"Submitting {run_id}: {identity['head']} / snapshot {identity['snapshot_sha256']}", flush=True)
            connection(config, "submit", run_id, input_path=packet)
        print(f"Run ID: {run_id}", flush=True)
        if args.detach:
            return 0
        last = None
        try:
            while True:
                state = connection(config, "status", run_id)["runs"][0]
                progress = (state["state"], state.get("phase"))
                if progress != last:
                    print(f"{run_id}: {progress[0]} / {progress[1] or 'preparing'}", flush=True)
                    last = progress
                if state["state"] in TERMINAL:
                    break
                time.sleep(5)
        except KeyboardInterrupt:
            connection(config, "cancel", run_id)
            print(f"Cancellation requested. Use status/fetch {run_id} for cleanup and logs.", file=sys.stderr)
            return 130
        report = fetch(config, run_id, args.results or local / "results" / run_id)
        print(f"Results: {args.results or local / 'results' / run_id}; remote run removed", flush=True)
        return report["exit_code"]
    run_id = valid_id(args.run_id) if args.run_id else None
    if args.command == "fetch":
        destination = args.results or local / "results" / run_id
        report = fetch(config, run_id, destination)
        print(f"Results: {destination}; remote run removed")
        return report["exit_code"]
    print(json.dumps(connection(config, "remove" if args.command == "clean" and run_id else args.command, run_id), indent=2))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RunnerError, OSError, ValueError) as error:
        print(f"remote-runner: {error}", file=sys.stderr)
        sys.exit(1)
