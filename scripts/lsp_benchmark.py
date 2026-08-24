#!/usr/bin/env python3

"""Reproducible end-to-end scalability benchmark for gti_lsp."""

import argparse
import json
import os
import pathlib
import platform
import select
import statistics
import subprocess
import sys
import time


def frame(message):
    payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
    return b"Content-Length: " + str(len(payload)).encode() + b"\r\n\r\n" + payload


def lsp_position(source, offset):
    prefix = source[:offset]
    line_text = prefix.rsplit("\n", 1)[-1]
    return {
        "line": prefix.count("\n"),
        "character": len(line_text.encode("utf-16-le")) // 2,
    }


def elapsed_ms(start):
    return (time.perf_counter_ns() - start) / 1_000_000.0


class LspSession:
    def __init__(self, executable, standard_library, trace_path, stderr_path):
        environment = os.environ.copy()
        environment["GTI_STDLIB_PATH"] = str(standard_library)
        environment["GTI_LSP_FRONTEND_TRACE"] = str(trace_path)
        self.stderr_file = stderr_path.open("wb")
        self.process = subprocess.Popen(
            [str(executable)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.stderr_file,
            env=environment,
        )
        self.buffer = b""
        self.messages = []
        self.stderr_path = stderr_path

    def send(self, message):
        self.process.stdin.write(frame(message))
        self.process.stdin.flush()

    def _read_one(self, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            header_end = self.buffer.find(b"\r\n\r\n")
            if header_end >= 0:
                headers = self.buffer[:header_end].decode("ascii").split("\r\n")
                content_length = next(
                    int(value.strip())
                    for name, value in (header.split(":", 1) for header in headers)
                    if name.lower() == "content-length"
                )
                payload_start = header_end + 4
                payload_end = payload_start + content_length
                if len(self.buffer) >= payload_end:
                    message = json.loads(self.buffer[payload_start:payload_end])
                    self.buffer = self.buffer[payload_end:]
                    return message

            remaining = deadline - time.monotonic()
            ready, _, _ = select.select(
                [self.process.stdout.fileno()], [], [], max(remaining, 0)
            )
            if not ready:
                break
            chunk = os.read(self.process.stdout.fileno(), 65536)
            if not chunk:
                break
            self.buffer += chunk
        return None

    def receive(self, predicate, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for index, message in enumerate(self.messages):
                if predicate(message):
                    return self.messages.pop(index)
            message = self._read_one(max(0, deadline - time.monotonic()))
            if message is None:
                break
            self.messages.append(message)
        stderr = self.stderr_path.read_text(encoding="utf-8", errors="replace")
        raise AssertionError(
            f"timed out waiting for LSP message; queued={self.messages}; "
            f"stderr={stderr}"
        )

    def initialize(self):
        self.send(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "capabilities": {
                        "textDocument": {
                            "hover": {"contentFormat": ["markdown", "plaintext"]},
                            "completion": {
                                "completionItem": {"snippetSupport": True}
                            },
                        }
                    }
                },
            }
        )
        self.receive(lambda message: message.get("id") == 1)
        self.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})

    def close(self):
        if self.process.poll() is None:
            self.send(
                {"jsonrpc": "2.0", "id": 9999, "method": "shutdown", "params": None}
            )
            self.receive(lambda message: message.get("id") == 9999)
            self.send({"jsonrpc": "2.0", "method": "exit", "params": None})
            self.process.wait(timeout=10)
        self.stderr_file.close()
        if self.process.returncode != 0:
            stderr = self.stderr_path.read_text(encoding="utf-8", errors="replace")
            raise AssertionError(
                f"gti_lsp exited with {self.process.returncode}: {stderr}"
            )


def diagnostics_for(uri, version=None):
    def matches(message):
        if message.get("method") != "textDocument/publishDiagnostics":
            return False
        params = message.get("params", {})
        return params.get("uri") == uri and (
            version is None or params.get("version") == version
        )

    return matches


def write_fixture(root, dependency_count):
    root.mkdir(parents=True, exist_ok=True)
    for index in range(dependency_count):
        (root / f"dep_{index:02d}.gti").write_text(
            f"int dep_{index:02d}(int value) {{ return value + {index}; }}\n",
            encoding="utf-8",
        )
    includes = "".join(
        f'#include "dep_{index:02d}.gti"\n' for index in range(dependency_count)
    )
    additions = "".join(
        f"  total = total + dep_{index:02d}({index + 1});\n"
        for index in range(dependency_count)
    )
    return (
        includes
        + "int benchmark_helper(int value) { return value + 1; }\n"
        + "int main() {\n"
        + "  mut int total = 0;\n"
        + additions
        + "  return benchmark_helper(total) + 0;\n"
        + "}\n"
    )


def full_change(uri, version, source):
    return {
        "jsonrpc": "2.0",
        "method": "textDocument/didChange",
        "params": {
            "textDocument": {"uri": uri, "version": version},
            "contentChanges": [{"text": source}],
        },
    }


def read_trace(path):
    if not path.exists():
        return []
    return [
        json.loads(line)
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]


def run_workload(executable, standard_library, fixture, run_root, dependency_count):
    source = write_fixture(fixture, dependency_count)
    root_path = (fixture / "main.gti").resolve()
    root_path.write_text(source, encoding="utf-8")
    dependency_path = (fixture / "dep_00.gti").resolve()
    dependency_source = dependency_path.read_text(encoding="utf-8")
    root_uri = root_path.as_uri()
    dependency_uri = dependency_path.as_uri()
    trace_path = run_root / "frontend-trace.jsonl"
    stderr_path = run_root / "lsp.stderr"
    session = LspSession(
        executable, standard_library, trace_path, stderr_path
    )
    timings = {}
    try:
        session.initialize()

        start = time.perf_counter_ns()
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": root_uri,
                        "languageId": "gti",
                        "version": 1,
                        "text": source,
                    }
                },
            }
        )
        cold = session.receive(diagnostics_for(root_uri, 1))
        timings["coldDiagnostics"] = elapsed_ms(start)
        assert not cold["params"]["diagnostics"], cold

        source = source.replace("benchmark_helper(total) + 0", "benchmark_helper(total) + 1")
        start = time.perf_counter_ns()
        session.send(full_change(root_uri, 2, source))
        repeated = session.receive(diagnostics_for(root_uri, 2))
        timings["repeatedEditDiagnostics"] = elapsed_ms(start)
        assert not repeated["params"]["diagnostics"], repeated

        start = time.perf_counter_ns()
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 10,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": root_uri}},
            }
        )
        tokens = session.receive(lambda message: message.get("id") == 10)
        timings["semanticTokens"] = elapsed_ms(start)
        assert tokens.get("result", {}).get("data"), tokens

        hover_offset = source.index("dep_00(", source.index("int total")) + 2
        start = time.perf_counter_ns()
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 11,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": root_uri},
                    "position": lsp_position(source, hover_offset),
                },
            }
        )
        hover = session.receive(lambda message: message.get("id") == 11)
        timings["hover"] = elapsed_ms(start)
        assert hover.get("result") is not None, hover

        completion_offset = source.index("benchmark_helper(total)") + len("benchmark_")
        start = time.perf_counter_ns()
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 12,
                "method": "textDocument/completion",
                "params": {
                    "textDocument": {"uri": root_uri},
                    "position": lsp_position(source, completion_offset),
                },
            }
        )
        completion = session.receive(lambda message: message.get("id") == 12)
        timings["completion"] = elapsed_ms(start)
        labels = {
            item["label"] for item in completion.get("result", {}).get("items", [])
        }
        assert "benchmark_helper" in labels, completion

        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": dependency_uri,
                        "languageId": "gti",
                        "version": 1,
                        "text": dependency_source,
                    }
                },
            }
        )
        dependency_open = session.receive(diagnostics_for(dependency_uri, 1))
        root_refresh = session.receive(diagnostics_for(root_uri, 2))
        assert not dependency_open["params"]["diagnostics"], dependency_open
        assert not root_refresh["params"]["diagnostics"], root_refresh

        changed_dependency = dependency_source.replace("value + 0", "value + 100")
        start = time.perf_counter_ns()
        session.send(full_change(dependency_uri, 2, changed_dependency))
        dirty = session.receive(diagnostics_for(root_uri, 2))
        timings["dirtyDependencyDiagnostics"] = elapsed_ms(start)
        assert not dirty["params"]["diagnostics"], dirty
        dependency_diagnostics = session.receive(diagnostics_for(dependency_uri, 2))
        assert not dependency_diagnostics["params"]["diagnostics"], dependency_diagnostics

        start = time.perf_counter_ns()
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 13,
                "method": "textDocument/completion",
                "params": {
                    "textDocument": {"uri": root_uri},
                    "position": lsp_position(source, completion_offset),
                },
            }
        )
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "$/cancelRequest",
                "params": {"id": 13},
            }
        )
        canceled = session.receive(lambda message: message.get("id") == 13)
        timings["cancellation"] = elapsed_ms(start)
        assert canceled.get("error", {}).get("code") == -32800, canceled

        start = time.perf_counter_ns()
        final_version = 6
        for version in range(3, final_version + 1):
            superseded_source = source.replace(
                "benchmark_helper(total) + 1",
                f"benchmark_helper(total) + {version}",
            )
            session.send(full_change(root_uri, version, superseded_source))
        final_diagnostics = session.receive(diagnostics_for(root_uri, final_version))
        timings["supersededDiagnostics"] = elapsed_ms(start)
        assert not final_diagnostics["params"]["diagnostics"], final_diagnostics
    finally:
        session.close()
    return {"timingsMilliseconds": timings, "frontendTrace": read_trace(trace_path)}


def summary(samples):
    result = {}
    for name in samples[0]["timingsMilliseconds"]:
        values = [sample["timingsMilliseconds"][name] for sample in samples]
        ordered = sorted(values)
        percentile_index = min(len(ordered) - 1, int(0.95 * len(ordered)))
        result[name] = {
            "medianMilliseconds": statistics.median(values),
            "p95Milliseconds": ordered[percentile_index],
            "samplesMilliseconds": values,
        }
    return result


def compare_to_baseline(current, baseline_path):
    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    baseline_summary = baseline["summary"]
    repeated_before = baseline_summary["repeatedEditDiagnostics"][
        "medianMilliseconds"
    ]
    repeated_after = current["repeatedEditDiagnostics"]["medianMilliseconds"]
    cold_before = baseline_summary["coldDiagnostics"]["medianMilliseconds"]
    cold_after = current["coldDiagnostics"]["medianMilliseconds"]
    repeated_reduction = (repeated_before - repeated_after) / repeated_before
    cold_regression = (cold_after - cold_before) / cold_before
    return {
        "baseline": str(baseline_path.resolve()),
        "repeatedEditReductionPercent": repeated_reduction * 100.0,
        "coldOpenRegressionPercent": cold_regression * 100.0,
        "targetMet": repeated_reduction >= 0.30 and cold_regression <= 0.10,
    }


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lsp", type=pathlib.Path, required=True)
    parser.add_argument(
        "--stdlib",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parent.parent / "stdlib",
    )
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--json", type=pathlib.Path)
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--dependencies", type=int, default=16)
    parser.add_argument("--baseline", type=pathlib.Path)
    parser.add_argument("--smoke", action="store_true")
    return parser.parse_args()


def main():
    args = parse_args()
    executable = args.lsp.resolve()
    standard_library = args.stdlib.resolve()
    if not executable.is_file():
        raise SystemExit(f"gti_lsp does not exist: {executable}")
    if not (standard_library / "prelude.gti").is_file():
        raise SystemExit(f"GTI standard library is incomplete: {standard_library}")
    runs = 1 if args.smoke else args.runs
    warmups = 0 if args.smoke else args.warmup
    if runs < 1 or warmups < 0 or args.dependencies < 2:
        raise SystemExit("runs must be positive; warmup non-negative; dependencies >= 2")

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    run_root = output / f"run-{time.time_ns()}-{os.getpid()}"
    run_root.mkdir()
    all_samples = []
    for iteration in range(warmups + runs):
        iteration_root = run_root / f"iteration-{iteration:03d}"
        iteration_root.mkdir()
        sample = run_workload(
            executable,
            standard_library,
            iteration_root / "fixture",
            iteration_root,
            args.dependencies,
        )
        if iteration >= warmups:
            all_samples.append(sample)

    version = subprocess.run(
        [str(executable), "--version"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    report = {
        "schema": 1,
        "benchmark": "gti-lsp-scalability",
        "lsp": str(executable),
        "lspVersion": version,
        "standardLibrary": str(standard_library),
        "machine": {
            "platform": platform.platform(),
            "processor": platform.processor(),
            "python": sys.version,
        },
        "configuration": {
            "runs": runs,
            "warmup": warmups,
            "dependencies": args.dependencies,
            "smoke": args.smoke,
        },
        "summary": summary(all_samples),
        "samples": all_samples,
    }
    if args.baseline:
        report["comparison"] = compare_to_baseline(report["summary"], args.baseline)
    report_path = args.json.resolve() if args.json else run_root / "report.json"
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(report_path)
    for name, values in report["summary"].items():
        print(f"{name}: median {values['medianMilliseconds']:.3f} ms")
    if "comparison" in report:
        comparison = report["comparison"]
        print(
            "repeated-edit reduction: "
            f"{comparison['repeatedEditReductionPercent']:.1f}%"
        )
        print(f"cold-open regression: {comparison['coldOpenRegressionPercent']:.1f}%")
        print(f"target met: {comparison['targetMet']}")


if __name__ == "__main__":
    main()
