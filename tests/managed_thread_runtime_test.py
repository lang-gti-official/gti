#!/usr/bin/env python3

"""Exercise the public <std/thread> vertical slice through the GTI CLI."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile


SUCCESS_SOURCE = """\
import std.thread;
import std.memory;

class resource {
  int32_t value;
public:
  resource(int32_t value) : value(value) {}
  int32_t read() { return this.value; }
};

class class_task {
public:
  class_task() = default;
  void operator()() { std::println("class read task"); }
  void operator()() mut { std::println("class mutable task"); }
  void operator()() && { std::println("class once task"); }
};

class fresh_task {
public:
  fresh_task() = default;
  void operator()() { std::println("fresh class task"); }
};

int main() {
  auto first_started = std::thread::start(
      []() -> void { std::println("first task"); });
  if (!first_started.has_value()) { return 1; }
  mut std::thread first = std::move(first_started).value();
  mut std::thread moved = std::move(first);
  auto first_join = moved.join();
  if (!first_join.has_value() || moved.joinable()) { return 2; }

  auto owner = std::make_unique<resource>(42);
  auto second_started = std::thread::start(
      [owned = std::move(owner)]() -> void {
        std::println(owned->read());
      });
  if (!second_started.has_value()) { return 3; }
  mut std::thread second = std::move(second_started).value();
  auto second_join = second.join();
  if (!second_join.has_value()) { return 4; }

  class_task task = class_task();
  auto third_started = std::thread::start(std::move(task));
  if (!third_started.has_value()) { return 5; }
  mut std::thread third = std::move(third_started).value();
  auto third_join = third.join();
  if (!third_join.has_value()) { return 6; }

  {
    auto fourth_started = std::thread::start(fresh_task());
    if (!fourth_started.has_value()) { return 7; }
    mut std::thread fourth = std::move(fourth_started).value();
  }
  std::println("after automatic join");
  return 0;
}
"""

TYPED_RESULT_SOURCE = """\
import std.thread;

class result_task {
  int32_t value;
public:
  result_task(int32_t value) : value(value) {}
  int32_t operator()() && { return this.value; }
};

int main() {
  mut std::thread_result<int32_t> worker =
      std::thread_result<int32_t>();
  auto started = worker.start(result_task(42));
  if (!started.has_value()) { return 1; }
  auto joined = worker.join();
  if (!joined.has_value() || joined.value() != 42) { return 2; }

  auto restarted = worker.start([]() -> int32_t { return 7; });
  if (!restarted.has_value()) { return 3; }
  auto rejoined = worker.join();
  if (!rejoined.has_value() || rejoined.value() != 7) { return 4; }
  return 0;
}
"""

TYPED_CLASS_RESULT_SOURCE = """\
import std.thread;

class result_value {
  int32_t value;
public:
  result_value(int32_t value) : value(value) {}
  result_value(result_value& other) = delete;
  result_value(result_value&& other) = default;
  int32_t read() { return this.value; }
};

int main() {
  mut std::thread_result<result_value> worker =
      std::thread_result<result_value>();
  auto started = worker.start([]() -> result_value {
    return result_value(42);
  });
  if (!started.has_value()) { return 1; }
  auto joined = worker.join();
  if (!joined.has_value()) { return 2; }
  mut result_value result = std::move(joined).value();
  if (result.read() != 42) { return 3; }
  return 0;
}
"""

TYPED_CLASS_RESULT_CLEANUP_SOURCE = """\
import std.thread;

[[unsafe_transfer]] class result_value {
  int32_t value;
public:
  result_value(int32_t value) : value(value) {}
  result_value(result_value& other) = delete;
  result_value(result_value&& other) = default;
  int32_t read() { return this.value; }
  ~result_value() { std::println(this.value); }
};

int main() {
  mut std::thread_result<result_value> worker =
      std::thread_result<result_value>();
  auto started = worker.start([]() -> result_value {
    return result_value(9);
  });
  if (!started.has_value()) { return 1; }
  auto joined = worker.join();
  if (!joined.has_value()) { return 2; }
  mut result_value result = std::move(joined).value();
  if (result.read() != 9) { return 3; }
  std::println("after extraction");
  return 0;
}
"""

TYPED_RESULT_CLEANUP_SOURCE = """\
import std.thread;

[[unsafe_transfer]] class result_value {
  int32_t value;
public:
  result_value(int32_t value) : value(value) {}
  result_value(result_value& other) = delete;
  result_value(result_value&& other) = default;
  ~result_value() { std::println(this.value); }
};

class result_task {
public:
  result_value operator()() && { return result_value(9); }
public:
  result_task() = default;
};

int main() {
  {
    mut std::thread_result<result_value> worker =
        std::thread_result<result_value>();
    auto started = worker.start(result_task());
    if (!started.has_value()) { return 1; }
  }
  std::println("after automatic result cleanup");
  return 0;
}
"""

TYPED_RESULT_FAILURE_SOURCE = """\
import std.thread;

class failing_result_task {
public:
  int32_t operator()() && {
    int32_t zero = 0;
    return 1 / zero;
  }
public:
  failing_result_task() = default;
};

int main() {
  mut std::thread_result<int32_t> worker =
      std::thread_result<int32_t>();
  auto started = worker.start(failing_result_task());
  if (!started.has_value()) { return 1; }
  auto joined = worker.join();
  return 0;
}
"""

CLASS_FAILURE_SOURCE = """\
import std.thread;

class failing_task {
public:
  void operator()() {
    int32_t zero = 0;
    int32_t value = 1 / zero;
  }
public:
  failing_task() = default;
};

int main() {
  failing_task task = failing_task();
  auto started = std::thread::start(std::move(task));
  if (!started.has_value()) { return 1; }
  mut std::thread worker = std::move(started).value();
  auto joined = worker.join();
  return 0;
}
"""

FAILURE_BODY = """\
import std.thread;

int main() {
  auto started = std::thread::start([]() -> void {
    int32_t zero = 0;
    int32_t value = 1 / zero;
  });
  if (!started.has_value()) { return 1; }
  mut std::thread worker = std::move(started).value();
%s
  return 0;
}
"""

CLEANUP_SOURCE = """\
import std.thread;

class first_cleanup {
public:
  ~first_cleanup() { std::println("first cleanup"); }
public:
  first_cleanup() = default;
};

class second_cleanup {
public:
  ~second_cleanup() { std::println("second cleanup"); }
public:
  second_cleanup() = default;
};

int main() {
  auto started = std::thread::start([]() -> void {
    first_cleanup first = first_cleanup();
    second_cleanup second = second_cleanup();
    int32_t zero = 0;
    int32_t value = 1 / zero;
  });
  if (!started.has_value()) { return 1; }
  mut std::thread worker = std::move(started).value();
  auto joined = worker.join();
  return 0;
}
"""

CLEANUP_EMERGENCY_SOURCE = """\
import std.thread;

class cleanup_failure {
public:
  ~cleanup_failure() {
    int32_t zero = 0;
    int32_t secondary = 1 / zero;
  }
public:
  cleanup_failure() = default;
};

int main() {
  auto started = std::thread::start([]() -> void {
    cleanup_failure guard = cleanup_failure();
    int32_t zero = 0;
    int32_t primary = 1 % zero;
  });
  if (!started.has_value()) { return 1; }
  mut std::thread worker = std::move(started).value();
  auto joined = worker.join();
  return 0;
}
"""

TASK_CLEANUP_EMERGENCY_SOURCE = """\
import std.thread;

[[unsafe_transfer]] class task_cleanup_failure {
public:
  ~task_cleanup_failure() {
    int32_t zero = 0;
    int32_t secondary = 1 / zero;
  }
public:
  task_cleanup_failure() = default;
};

int main() {
  task_cleanup_failure guard = task_cleanup_failure();
  auto task = [owned = std::move(guard)]() -> void {
    int32_t zero = 0;
    int32_t primary = 1 % zero;
  };
  auto started = std::thread::start(std::move(task));
  if (!started.has_value()) { return 1; }
  mut std::thread worker = std::move(started).value();
  auto joined = worker.join();
  return 0;
}
"""

TASK_CLEANUP_FAILURE_SOURCE = """\
import std.thread;

[[unsafe_transfer]] class task_cleanup_failure {
public:
  ~task_cleanup_failure() {
    int32_t zero = 0;
    int32_t value = 1 / zero;
  }
public:
  task_cleanup_failure() = default;
};

int main() {
  task_cleanup_failure guard = task_cleanup_failure();
  auto task = [owned = std::move(guard)]() -> void {};
  auto started = std::thread::start(std::move(task));
  if (!started.has_value()) { return 1; }
  mut std::thread worker = std::move(started).value();
  auto joined = worker.join();
  return 0;
}
"""

TASK_INTERNAL_CLEANUP_EMERGENCY_SOURCE = """\
import std.thread;

class secondary_cleanup_failure {
public:
  ~secondary_cleanup_failure() {
    int32_t zero = 0;
    int32_t secondary = 1 / zero;
  }
public:
  secondary_cleanup_failure() = default;
};

[[unsafe_transfer]] class task_internal_cleanup_failure {
public:
  ~task_internal_cleanup_failure() {
    secondary_cleanup_failure cleanup = secondary_cleanup_failure();
    int32_t zero = 0;
    int32_t primary = 1 % zero;
  }
public:
  task_internal_cleanup_failure() = default;
};

int main() {
  task_internal_cleanup_failure guard = task_internal_cleanup_failure();
  auto task = [owned = std::move(guard)]() -> void {};
  auto started = std::thread::start(std::move(task));
  if (!started.has_value()) { return 1; }
  mut std::thread worker = std::move(started).value();
  auto joined = worker.join();
  return 0;
}
"""

STRESS_SOURCE = """\
import std.thread;

int main() {
  for (mut int32_t index = 0; index < 128; index++) {
    auto started = std::thread::start([]() -> void {});
    if (!started.has_value()) { return 1; }
    mut std::thread worker = std::move(started).value();
    auto joined = worker.join();
    if (!joined.has_value()) { return 2; }
  }
  return 0;
}
"""

AUTOMATIC_JOIN_HOST_FAILURE_SOURCE = """\
import std.thread;
import std.mutex;

std::mutex<int32_t> process_value{7};

extern "C" {
  void gti_test_inject_thread_join_failure();
  void gti_test_expect_native_storage_pinned(int32_t value);
}

class CleanupProbe {
public:
  ~CleanupProbe() {
    auto held = process_value.lock();
    gti_test_expect_native_storage_pinned(held.get());
  }
public:
  CleanupProbe() = default;
};

int main() {
  CleanupProbe probe{};
  auto started = std::thread::start([]() -> void {
    auto held = process_value.lock();
  });
  if (!started.has_value()) { return 1; }
  mut std::thread worker = std::move(started).value();
  gti_test_inject_thread_join_failure();
  return 0;
}
"""

NESTED_FAILURE_SOURCE = """\
import std.thread;

int main() {
  auto outer_started = std::thread::start([]() -> void {
    auto inner_started = std::thread::start([]() -> void {
      int32_t zero = 0;
      int32_t value = 1 / zero;
    });
    if (!inner_started.has_value()) { return; }
    mut std::thread inner = std::move(inner_started).value();
    auto inner_join = inner.join();
  });
  if (!outer_started.has_value()) { return 1; }
  mut std::thread outer = std::move(outer_started).value();
  auto outer_join = outer.join();
  return 0;
}
"""


def execute(
    command: list[str],
    *,
    expected: int = 0,
    cwd: pathlib.Path | None = None,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        cwd=cwd,
        capture_output=True,
        text=True,
        timeout=300,
        check=False,
    )
    if result.returncode != expected:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected}\n"
            f"command: {command}\nstdout: {result.stdout}\nstderr: {result.stderr}"
        )
    return result


def executable_path(root: pathlib.Path, name: str) -> pathlib.Path:
    return root / (f"{name}.exe" if sys.platform == "win32" else name)


def build(
    compiler: pathlib.Path,
    root: pathlib.Path,
    name: str,
    source: str,
    *,
    standard: str = "c++23",
    optimization: str = "-O0",
    native_arguments: list[str] | None = None,
) -> pathlib.Path:
    source_path = root / f"{name}.gti"
    binary = executable_path(root, name)
    source_path.write_text(source, encoding="utf-8")
    command = [
        str(compiler),
        "--execution-profile",
        "concurrent",
        "--std",
        standard,
        optimization,
        str(source_path),
        "-o",
        str(binary),
    ]
    if native_arguments:
        command.extend(["--", *native_arguments])
    execute(command)
    return binary


def source_line(source: str, needle: str) -> int:
    return next(
        index
        for index, line in enumerate(source.splitlines(), start=1)
        if needle in line
    )


def expect_process(
    binary: pathlib.Path,
    *,
    code: int,
    stdout: str,
    stderr: str,
) -> None:
    result = execute([str(binary)], expected=code)
    if result.stdout != stdout or result.stderr != stderr:
        raise AssertionError(
            f"unexpected output from {binary.name}\n"
            f"stdout: {result.stdout!r}\nstderr: {result.stderr!r}"
        )


def test_success_matrix(
    compiler: pathlib.Path, root: pathlib.Path
) -> None:
    expected = (
        "first task\n42\nclass once task\nfresh class task\n"
        "after automatic join\n"
    )
    for standard in ("c++20", "c++23"):
        for optimization in ("-O0", "-O3"):
            suffix = f"{standard.replace('+', 'p')}_{optimization[1:]}"
            binary = build(
                compiler,
                root,
                f"managed_thread_{suffix}",
                SUCCESS_SOURCE,
                standard=standard,
                optimization=optimization,
            )
            expect_process(binary, code=0, stdout=expected, stderr="")


def test_typed_result_matrix(
    compiler: pathlib.Path, root: pathlib.Path
) -> None:
    for standard in ("c++20", "c++23"):
        for optimization in ("-O0", "-O3"):
            suffix = f"{standard.replace('+', 'p')}_{optimization[1:]}"
            binary = build(
                compiler,
                root,
                f"managed_thread_result_{suffix}",
                TYPED_RESULT_SOURCE,
                standard=standard,
                optimization=optimization,
            )
            expect_process(binary, code=0, stdout="", stderr="")


def test_typed_class_result_matrix(
    compiler: pathlib.Path, root: pathlib.Path
) -> None:
    for standard in ("c++20", "c++23"):
        for optimization in ("-O0", "-O3"):
            suffix = f"{standard.replace('+', 'p')}_{optimization[1:]}"
            binary = build(
                compiler,
                root,
                f"managed_thread_class_result_{suffix}",
                TYPED_CLASS_RESULT_SOURCE,
                standard=standard,
                optimization=optimization,
            )
            expect_process(binary, code=0, stdout="", stderr="")


def test_typed_class_result_cleanup(
    compiler: pathlib.Path, root: pathlib.Path
) -> None:
    for standard, optimization in (("c++20", "-O0"), ("c++23", "-O3")):
        suffix = f"{standard.replace('+', 'p')}_{optimization[1:]}"
        binary = build(
            compiler,
            root,
            f"managed_thread_class_result_cleanup_{suffix}",
            TYPED_CLASS_RESULT_CLEANUP_SOURCE,
            standard=standard,
            optimization=optimization,
        )
        expect_process(
            binary,
            code=0,
            stdout="after extraction\n9\n",
            stderr="",
        )


def test_typed_result_cleanup(
    compiler: pathlib.Path, root: pathlib.Path
) -> None:
    binary = build(
        compiler,
        root,
        "managed_thread_result_cleanup",
        TYPED_RESULT_CLEANUP_SOURCE,
        standard="c++23",
        optimization="-O3",
    )
    expect_process(
        binary,
        code=0,
        stdout="9\nafter automatic result cleanup\n",
        stderr="",
    )


def test_typed_result_failure(
    compiler: pathlib.Path, root: pathlib.Path
) -> None:
    name = "managed_thread_result_failure"
    source = TYPED_RESULT_FAILURE_SOURCE
    binary = build(compiler, root, name, source)
    line = source_line(source, "return 1 / zero;")
    expected = (
        f"{name}.gti:{line}: runtime error[GTI-R0002]: "
        "division_by_zero in integer_division\n"
    )
    expect_process(binary, code=70, stdout="", stderr=expected)


def test_failure_handoff(
    compiler: pathlib.Path, root: pathlib.Path
) -> None:
    explicit = FAILURE_BODY % "  auto joined = worker.join();"
    automatic = FAILURE_BODY % ""
    for name, source in (
        ("managed_thread_explicit_failure", explicit),
        ("managed_thread_automatic_failure", automatic),
    ):
        binary = build(compiler, root, name, source)
        line = source_line(source, "int32_t value = 1 / zero;")
        expected = (
            f"{name}.gti:{line}: runtime error[GTI-R0002]: "
            "division_by_zero in integer_division\n"
        )
        expect_process(binary, code=70, stdout="", stderr=expected)


def test_class_failure_handoff(
    compiler: pathlib.Path, root: pathlib.Path
) -> None:
    name = "managed_thread_class_failure"
    binary = build(compiler, root, name, CLASS_FAILURE_SOURCE)
    line = source_line(CLASS_FAILURE_SOURCE, "int32_t value = 1 / zero;")
    expected = (
        f"{name}.gti:{line}: runtime error[GTI-R0002]: "
        "division_by_zero in integer_division\n"
    )
    expect_process(binary, code=70, stdout="", stderr=expected)


def test_cleanup_order(
    compiler: pathlib.Path, root: pathlib.Path
) -> None:
    name = "managed_thread_cleanup"
    binary = build(compiler, root, name, CLEANUP_SOURCE)
    line = source_line(CLEANUP_SOURCE, "int32_t value = 1 / zero;")
    expected = (
        f"{name}.gti:{line}: runtime error[GTI-R0002]: "
        "division_by_zero in integer_division\n"
    )
    expect_process(
        binary,
        code=70,
        stdout="second cleanup\nfirst cleanup\n",
        stderr=expected,
    )


def test_cleanup_emergency(
    compiler: pathlib.Path, root: pathlib.Path
) -> None:
    name = "managed_thread_cleanup_emergency"
    binary = build(compiler, root, name, CLEANUP_EMERGENCY_SOURCE)
    primary = source_line(CLEANUP_EMERGENCY_SOURCE, "int32_t primary =")
    secondary = source_line(CLEANUP_EMERGENCY_SOURCE, "int32_t secondary =")
    expected = (
        f"{name}.gti:{secondary}: runtime error[GTI-R0014]: "
        "failure_during_cleanup; primary [GTI-R0003] at "
        f"{name}.gti:{primary}; secondary [GTI-R0002]\n"
    )
    expect_process(binary, code=70, stdout="", stderr=expected)


def test_task_cleanup_emergency(
    compiler: pathlib.Path, root: pathlib.Path
) -> None:
    name = "managed_thread_task_cleanup_emergency"
    source = TASK_CLEANUP_EMERGENCY_SOURCE
    binary = build(compiler, root, name, source)
    primary = source_line(source, "int32_t primary =")
    secondary = source_line(source, "int32_t secondary =")
    expected = (
        f"{name}.gti:{secondary}: runtime error[GTI-R0014]: "
        "failure_during_cleanup; primary [GTI-R0003] at "
        f"{name}.gti:{primary}; secondary [GTI-R0002]\n"
    )
    expect_process(binary, code=70, stdout="", stderr=expected)


def test_task_cleanup_failure(
    compiler: pathlib.Path, root: pathlib.Path
) -> None:
    name = "managed_thread_task_cleanup_failure"
    source = TASK_CLEANUP_FAILURE_SOURCE
    binary = build(compiler, root, name, source)
    line = source_line(source, "int32_t value = 1 / zero;")
    expected = (
        f"{name}.gti:{line}: runtime error[GTI-R0002]: "
        "division_by_zero in integer_division\n"
    )
    expect_process(binary, code=70, stdout="", stderr=expected)


def test_task_internal_cleanup_emergency(
    compiler: pathlib.Path, root: pathlib.Path
) -> None:
    name = "managed_thread_task_internal_cleanup_emergency"
    source = TASK_INTERNAL_CLEANUP_EMERGENCY_SOURCE
    binary = build(compiler, root, name, source)
    primary = source_line(source, "int32_t primary =")
    secondary = source_line(source, "int32_t secondary =")
    expected = (
        f"{name}.gti:{secondary}: runtime error[GTI-R0014]: "
        "failure_during_cleanup; primary [GTI-R0003] at "
        f"{name}.gti:{primary}; secondary [GTI-R0002]\n"
    )
    expect_process(binary, code=70, stdout="", stderr=expected)


def test_stress(compiler: pathlib.Path, root: pathlib.Path) -> None:
    binary = build(
        compiler,
        root,
        "managed_thread_stress",
        STRESS_SOURCE,
        standard="c++20",
        optimization="-O3",
    )
    expect_process(binary, code=0, stdout="", stderr="")


def test_automatic_join_host_failure(
    compiler: pathlib.Path, root: pathlib.Path
) -> None:
    helper = root / "managed-thread-fault.cpp"
    helper.write_text(
        '#include "thread_internal.h"\n'
        '#include "gti/runtime_thread.h"\n'
        '#include <cstdlib>\n'
        'extern "C" void gti_test_inject_thread_join_failure() noexcept {\n'
        "  gti::runtime::detail::setThreadTestFault(\n"
        "      gti::runtime::detail::ThreadTestFault::JoinFailed);\n"
        "}\n"
        'extern "C" void gti_test_expect_native_storage_pinned(int32_t value) noexcept {\n'
        "  if (value != 7 || gti_rt_thread_program_storage_retirement_ready_v1()) std::abort();\n"
        "}\n",
        encoding="utf-8",
    )
    source_root = pathlib.Path(__file__).resolve().parent.parent
    for standard, optimization in (("c++20", "-O0"), ("c++23", "-O3")):
        binary = build(
            compiler,
            root,
            f"managed_thread_automatic_join_host_failure_{standard}_{optimization[1:]}",
            AUTOMATIC_JOIN_HOST_FAILURE_SOURCE,
            standard=standard,
            optimization=optimization,
            native_arguments=[
                str(helper),
                f"-I{source_root / 'runtime/src'}",
            ],
        )
        result = execute([str(binary)], expected=70)
        if result.stdout != "" or "runtime error[GTI-R0012]" not in result.stderr or (
            "infallible_host_operation_failed in automatic_join\n"
            not in result.stderr
        ):
            raise AssertionError(
                "automatic join did not re-raise its exact GTI-R0012 outcome\n"
                f"stdout: {result.stdout!r}\nstderr: {result.stderr!r}"
            )


def test_nested_worker_failure(
    compiler: pathlib.Path, root: pathlib.Path
) -> None:
    name = "managed_thread_nested_failure"
    source = NESTED_FAILURE_SOURCE
    binary = build(compiler, root, name, source)
    line = source_line(source, "int32_t value = 1 / zero;")
    expected = (
        f"{name}.gti:{line}: runtime error[GTI-R0002]: "
        "division_by_zero in integer_division\n"
    )
    expect_process(binary, code=70, stdout="", stderr=expected)


def test_project_profile(
    compiler: pathlib.Path, root: pathlib.Path
) -> None:
    project = root / "managed-thread-project"
    source = project / "src/main.gti"
    source.parent.mkdir(parents=True)
    source.write_text(SUCCESS_SOURCE, encoding="utf-8")
    project.joinpath("gti.toml").write_text(
        """\
manifest-version = 1

[package]
name = "managed_thread_project"
version = "0.1.0"

[targets.managed_thread_project]
kind = "executable"
root = "src/main.gti"

[profiles.dev]
optimization = 3
cpp-standard = "c++20"
execution-profile = "concurrent"
""",
        encoding="utf-8",
    )
    result = execute([str(compiler), "run"], cwd=project)
    if result.stdout != (
        "first task\n42\nclass once task\nfresh class task\n"
        "after automatic join\n"
    ):
        raise AssertionError(
            f"project profile produced unexpected stdout: {result.stdout!r}"
        )


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: managed_thread_runtime_test.py /path/to/gti")
    requested_compiler = pathlib.Path(sys.argv[1])
    resolved_compiler = (
        requested_compiler.resolve()
        if requested_compiler.exists()
        else shutil.which(sys.argv[1])
    )
    if resolved_compiler is None:
        raise AssertionError(f"could not resolve GTI executable: {sys.argv[1]}")
    compiler = pathlib.Path(resolved_compiler).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-managed-thread-") as scratch:
        root = pathlib.Path(scratch)
        test_success_matrix(compiler, root)
        test_typed_result_matrix(compiler, root)
        test_typed_class_result_matrix(compiler, root)
        test_typed_class_result_cleanup(compiler, root)
        test_typed_result_cleanup(compiler, root)
        test_typed_result_failure(compiler, root)
        test_failure_handoff(compiler, root)
        test_class_failure_handoff(compiler, root)
        test_cleanup_order(compiler, root)
        test_cleanup_emergency(compiler, root)
        test_task_cleanup_failure(compiler, root)
        test_task_cleanup_emergency(compiler, root)
        test_task_internal_cleanup_emergency(compiler, root)
        test_stress(compiler, root)
        test_automatic_join_host_failure(compiler, root)
        test_nested_worker_failure(compiler, root)
        test_project_profile(compiler, root)
    print("managed thread end-to-end checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
