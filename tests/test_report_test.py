#!/usr/bin/env python3
"""Parse JSON Lines independently, including hostile output and non-C locales."""
import json
import subprocess
import sys


def main():
    result = subprocess.run([sys.argv[1]], capture_output=True, check=True, timeout=15)
    assert result.stderr == b"", result.stderr
    # Strict UTF-8 decoding and JSON parsing catch invalid output/injected events.
    events = [json.loads(line) for line in result.stdout.decode("utf-8").splitlines()]
    assert len(events) == 10, events
    assert all(event["schema_version"] == 1 for event in events)
    inventory, started, passed = events[:3]
    assert inventory == {
        "schema_version": 1, "event": "suite_discovered", "target": 'suite"\\',
        "tests": [{"name": "app::unicode_é", "selected": True, "skip_reason": None},
                  {"name": "app::filtered", "selected": False, "skip_reason": "filtered"},
                  {"name": "app::skipped", "selected": False, "skip_reason": "skipped"}]}
    assert started == {"schema_version": 1, "event": "test_started",
                       "target": 'suite"\\', "name": "app::unicode_é"}
    assert passed["status"] == "passed" and passed["exit_code"] == 0
    assert passed["signal"] is None and passed["diagnostic"] is None
    assert passed["duration_ms"] == 1234567
    assert passed["output"] == 'quote" slash\\ newline\n tab\t null\0 unicode é 🐈 invalid ' + "�" * 10
    assert [event["status"] for event in events[3:8]] == [
        "failed", "crashed", "timed_out", "runner_error", "skipped"]
    for event in events[3:8]:
        assert event["event"] == "test_finished"
        assert event["duration_ms"] == 2**64 - 1
        assert event["signal"] == 9 and event["exit_code"] == 137
        assert event["diagnostic"] == 'line\n"message"'
    summary, listing = events[-2:]
    assert summary == {
        "schema_version": 1, "event": "run_finished", "discovered": 10000,
        "selected": 9999, "started": 9999, "completed": 9999, "passed": 9995,
        "failed": 1, "crashed": 1, "timed_out": 1, "runner_errors": 1,
        "skipped": 1, "list_only": False, "complete": True,
        "exit_code": 70, "diagnostic": None}
    assert listing["list_only"] is True and listing["complete"] is True
    assert listing["started"] == listing["completed"] == 0
    assert listing["diagnostic"] == "No selected tests"


if __name__ == "__main__":
    main()
