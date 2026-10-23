#!/usr/bin/env python3
"""External oracles for selection, process outcomes, and complete JSON events."""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile


def main():
    compiler = Path(sys.argv[1]).resolve()
    root = Path(__file__).resolve().parents[1]
    env = dict(os.environ, GTI_STDLIB_PATH=str(root / "stdlib"), NO_COLOR="1")
    for key in ("GTI_CXX", "CXX", "CPATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH"):
        env.pop(key, None)
    with tempfile.TemporaryDirectory(prefix="gti-test-controls-") as temporary:
        project = Path(temporary) / "project"
        shutil.copytree(root / "examples/annotated-tests", project,
                        ignore=shutil.ignore_patterns("build"))
        source = project / "tests/main.gti"
        source.write_text('''#include <std/testing>
#include <std/string>
extern "C" { int raise(int signal); int usleep(uint32_t duration); }
[[test]] void a_pass() { std::println("pass-output"); }
[[test]] void b_failure() { std::testing::check(false, "forced-failure"); }
[[test]] void c_crash() { unsafe { [[discard]] raise(15); } }
[[test]] void d_timeout() { unsafe { [[discard]] usleep(uint32_t(5000000)); } }
[[test]] void e_after() { std::println("after-output"); }
''')

        def run(*args, expected=0, structured=True):
            result = subprocess.run([str(compiler), "test", "--progress", "never",
                                     *args, *(["--format", "json"] if structured else [])],
                                    cwd=project, env=env, capture_output=True, text=True,
                                    timeout=180)
            assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
            if not structured:
                return result
            events = [json.loads(line) for line in result.stdout.splitlines()]
            assert events and all(e["schema_version"] == 1 for e in events), events
            assert events[-1]["event"] == "run_finished", events
            return events

        for level in ("-O0", "-O3"):
            events = run(level, "--list")
            summary = events[-1]
            assert summary["selected"] == 5 and summary["started"] == summary["completed"] == 0
            assert summary["list_only"] and summary["complete"]
            assert [e["event"] for e in events] == ["suite_discovered", "run_finished"]
            events = run(level, "--jobs", "2", "--timeout", "2s", expected=70)
            inventory = events[0]["tests"]
            started = [e["name"] for e in events if e["event"] == "test_started"]
            finished = [e for e in events if e["event"] == "test_finished"]
            names = [t["name"] for t in inventory if t["selected"]]
            assert sorted(started) == sorted(names) == sorted(e["name"] for e in finished)
            statuses = {e["name"]: e["status"] for e in finished}
            assert statuses == {"a_pass": "passed", "b_failure": "failed", "c_crash": "crashed",
                                "d_timeout": "timed_out", "e_after": "passed"}, statuses
            assert "forced-failure" in next(e["output"] for e in finished if e["name"] == "b_failure")
            summary = events[-1]
            assert summary["complete"] and summary["selected"] == summary["started"] == summary["completed"] == 5
            assert [summary[k] for k in ("passed", "failed", "crashed", "timed_out", "runner_errors")] == [2, 1, 1, 1, 0]
            # The same cached suite supports different selections without rebuilding its catalog.
            events = run(level, "--filter", "a_pass", "--exact", "--timeout", "0")
            assert events[-1]["passed"] == 1 and events[-1]["skipped"] == 4
            events = run(level, "--filter", "pass", "--skip", "a_pass", expected=1)
            assert not events[-1]["complete"] and events[-1]["selected"] == 0
            assert events[0]["tests"][0]["skip_reason"] == "skipped"
            events = run(level, "--filter", "absent", "--allow-empty")
            assert events[-1]["complete"] and events[-1]["selected"] == 0
            listing = run(level, "--list", "--filter", "after", structured=False)
            assert "e_after" in listing.stdout and "a_pass" not in listing.stdout

        # Interrupt after the runner's launch event. The parent must reap the
        # isolated child, emit an incomplete summary, and preserve signal status.
        for interruption in (signal.SIGINT, signal.SIGTERM):
            with tempfile.TemporaryFile() as diagnostics:
                child = subprocess.Popen([str(compiler), "test", "--filter", "d_timeout", "--exact",
                                          "--timeout", "0", "--format", "json"], cwd=project,
                                         env=env, stdout=subprocess.PIPE, stderr=diagnostics,
                                         text=True)
                try:
                    prefix = []
                    while True:
                        line = child.stdout.readline()
                        assert line, "runner ended before starting the selected test"
                        prefix.append(line)
                        if json.loads(line)["event"] == "test_started":
                            break
                    child.send_signal(interruption)
                    output, _ = child.communicate(timeout=15)
                    interrupted = [json.loads(line) for line in ("".join(prefix) + output).splitlines()]
                    assert child.returncode == 128 + interruption, interrupted
                    assert not interrupted[-1]["complete"] and interrupted[-1]["completed"] == 1
                    assert next(e for e in interrupted if e["event"] == "test_finished")["status"] == "runner_error"
                finally:
                    if child.poll() is None:
                        child.kill()
                        child.wait()

        for args in (("--jobs", "0"), ("--timeout", "-1s"), ("--timeout", "2"),
                     ("--timeout", "999999999999999999999m"), ("--exact",),
                     ("--filter", ""), ("--format", "xml")):
            run(*args, expected=64, structured=False)
        source.write_text("[[test]] void invalid(int value) {}\n")
        events = run(expected=65)
        assert len(events) == 1 and not events[-1]["complete"] and events[-1]["runner_errors"] == 1
        source.write_text('int main() { return 23; }\n')
        events = run(expected=23)
        assert events[-1]["discovered"] == events[-1]["completed"] == 1
        assert next(e for e in events if e["event"] == "test_finished")["status"] == "failed"


if __name__ == "__main__":
    main()
