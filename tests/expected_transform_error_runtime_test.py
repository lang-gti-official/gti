#!/usr/bin/env python3

"""Consuming error mapping preserves branch effects and exact owning cleanup."""

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile


SOURCE = r"""
mut int mapper_calls = 0;

enum class source_errc {
  None = 0,
  Broken = 1,
};

enum class app_errc {
  None = 0,
  RequestFailed = 1,
};

app_errc map_source_error(source_errc error) {
  mapper_calls += 1;
  if (error == source_errc::Broken) {
    return app_errc::RequestFailed;
  }
  return app_errc::None;
}

mut int second_mapper_calls = 0;
int map_app_error(app_errc error) {
  second_mapper_calls += 1;
  return error == app_errc::RequestFailed ? 9 : 0;
}

expected<int, source_errc> read_number(bool fail) {
  if (fail) { return unexpected(source_errc::Broken); }
  return 42;
}

expected<int, app_errc> propagated(bool fail) {
  return ?read_number(fail).transform_error(map_source_error);
}

expected<int, app_errc> direct_return(bool fail) {
  return read_number(fail).transform_error(map_source_error);
}

int inspect(expected<int, app_errc> value) {
  return value.has_value() ? value.value() : 9;
}

expected<int, app_errc> propagated_named(bool fail) {
  mut auto result = read_number(fail);
  return ?std::move(result).transform_error(map_source_error);
}

class Payload {
public:
  mut int value = 0;

  Payload(int initial) : value(initial) {}
  Payload(Payload& other) = delete;
  Payload(Payload&& other) = default;
public:
  Payload() = default;
};

expected<Payload, source_errc> read_payload(bool fail) {
  if (fail) { return unexpected(source_errc::Broken); }
  return Payload(51);
}

expected<Payload, app_errc> propagated_payload(bool fail) {
  return ?read_payload(fail).transform_error(map_source_error);
}

expected<void, source_errc> perform(bool fail) {
  if (fail) { return unexpected(source_errc::Broken); }
  return;
}

expected<void, app_errc> propagated_void(bool fail) {
  ?perform(fail).transform_error(map_source_error);
  return;
}

int main() {
  auto direct_success = read_number(false).transform_error(map_source_error);
  if (!direct_success.has_value() || direct_success.value() != 42 ||
      mapper_calls != 0) {
    return 1;
  }

  auto direct_failure = read_number(true).transform_error(map_source_error);
  if (direct_failure.has_value() ||
      direct_failure.error() != app_errc::RequestFailed ||
      mapper_calls != 1) {
    return 2;
  }

  auto propagated_success = propagated(false);
  if (!propagated_success.has_value() || propagated_success.value() != 42 ||
      mapper_calls != 1) {
    return 3;
  }

  auto propagated_failure = propagated(true);
  if (propagated_failure.has_value() ||
      propagated_failure.error() != app_errc::RequestFailed ||
      mapper_calls != 2) {
    return 4;
  }

  auto named_success = propagated_named(false);
  auto named_failure = propagated_named(true);
  if (!named_success.has_value() || named_success.value() != 42 ||
      named_failure.has_value() ||
      named_failure.error() != app_errc::RequestFailed ||
      mapper_calls != 3) {
    return 5;
  }

  auto payload_success = propagated_payload(false);
  auto payload_failure = propagated_payload(true);
  if (!payload_success.has_value() || payload_success.value().value != 51 ||
      payload_failure.has_value() ||
      payload_failure.error() != app_errc::RequestFailed ||
      mapper_calls != 4) {
    return 6;
  }

  auto void_success = propagated_void(false);
  auto void_failure = propagated_void(true);
  if (!void_success.has_value() || void_failure.has_value() ||
      void_failure.error() != app_errc::RequestFailed ||
      mapper_calls != 5) {
    return 7;
  }
  auto returned = direct_return(true);
  if (returned.has_value() || mapper_calls != 6) { return 8; }
  if (inspect(read_number(false).transform_error(map_source_error)) != 42 ||
      inspect(read_number(true).transform_error(map_source_error)) != 9 ||
      mapper_calls != 7) { return 9; }
  auto chained_success = read_number(false).transform_error(map_source_error)
      .transform_error(map_app_error);
  auto chained_failure = read_number(true).transform_error(map_source_error)
      .transform_error(map_app_error);
  if (!chained_success.has_value() || chained_success.value() != 42 ||
      chained_failure.has_value() || chained_failure.error() != 9 ||
      mapper_calls != 8 || second_mapper_calls != 1) { return 10; }
  for (mut int index = 0; index < 4; index += 1) {
    auto repeated = read_number(index % 2 == 1).transform_error(map_source_error);
    if (repeated.has_value() != (index % 2 == 0)) { return 11; }
  }
  if (mapper_calls != 10) { return 12; }
  auto named_payload = read_payload(false);
  auto named_payload_result = std::move(named_payload).transform_error(map_source_error);
  auto named_payload_error = read_payload(true);
  auto named_payload_failure = std::move(named_payload_error).transform_error(map_source_error);
  if (!named_payload_result.has_value() || named_payload_result.value().value != 51 ||
      named_payload_failure.has_value() || mapper_calls != 11) { return 13; }
  for (mut int index = 0; index < 4; index += 1) {
    auto repeated = read_payload(index % 2 == 1).transform_error(map_source_error);
    if (repeated.has_value() != (index % 2 == 0)) { return 14; }
    if (repeated.has_value() && repeated.value().value != 51) { return 15; }
  }
  if (mapper_calls != 13) { return 16; }
  return 0;
}
"""


OWNED_SOURCE = r"""import std.string;
import std.vector;

class Guard {
  int identity;
public:
  Guard(int identity) : identity(identity) {}
  ~Guard() { std::println(this.identity); }
};

class Payload {
  std::string text;
public:
  Payload(std::string text) : text(std::move(text)) {}
  ~Payload() { std::println(30); }
  std::size_t size() { return this.text.size(); }
};

class SourceError {
  int divisor;
  std::string text;
public:
  SourceError(int divisor, std::string text)
      : divisor(divisor), text(std::move(text)) {}
  ~SourceError() {
    std::println(10);
    if (this.divisor < 0) { int checked = 1 / (this.divisor + 1); }
  }
  int denominator() { return this.divisor; }
  std::size_t size() { return this.text.size(); }
};

class AppError {
  std::string text;
public:
  AppError(std::string text) : text(std::move(text)) {}
  ~AppError() { std::println(20); }
  std::size_t size() { return this.text.size(); }
};

mut int mapper_calls = 0;
AppError map_owned(SourceError error) {
  Guard local{40};
  mapper_calls += 1;
  std::println(1);
  int checked = 1 / error.denominator();
  if (error.size() != std::size_t(6)) { return AppError(std::string("bad")); }
  return AppError(std::string("mapped"));
}

int map_again(AppError error) {
  std::println(4);
  return error.size() == std::size_t(6) ? 9 : 0;
}

struct DefaultError {
  int code = 4;
  ~DefaultError() { std::println(50); }
public:
  DefaultError() = default;
};
DefaultError map_default(SourceError error) { return DefaultError(); }

expected<Payload, SourceError> produce(bool fail, int divisor) {
  if (fail) { return unexpected(SourceError(divisor, std::string("source"))); }
  return Payload(std::string("payload"));
}

expected<void, SourceError> perform(bool fail) {
  if (fail) { return unexpected(SourceError(1, std::string("source"))); }
  return;
}

expected<Payload, AppError> returned(bool fail) {
  return produce(fail, 1).transform_error(map_owned);
}

expected<Payload, AppError> propagated(bool fail) {
  return ?produce(fail, 1).transform_error(map_owned);
}

int consume(expected<Payload, AppError> result, int extra) {
  std::println(3);
  if (!result.has_value() && result.error().size() == std::size_t(6)) {
    return extra;
  }
  return 99;
}

int later(int divisor) { return 12 / divisor; }

int main(int argc, std::vector<std::string> argv) {
  Guard caller{99};
  if (argc <= 3) {
    auto result = produce(argc != 1, argc == 3 ? 0 : 1).transform_error(map_owned);
    std::println(2);
    if (argc == 1) {
      if (!result.has_value() || result.value().size() != std::size_t(7) ||
          mapper_calls != 0) { return 1; }
    } else if (result.has_value() || result.error().size() != std::size_t(6) ||
               mapper_calls != 1) { return 2; }
  } else if (argc == 4) {
    auto result = returned(true);
    std::println(2);
    if (result.has_value() || result.error().size() != std::size_t(6)) { return 3; }
  } else if (argc == 5 || argc == 6) {
    int result = consume(produce(true, 1).transform_error(map_owned),
                         later(argc == 6 ? 0 : 1));
    if (result != 12) { return 4; }
  } else if (argc == 7 || argc == 8) {
    auto result = perform(argc == 8).transform_error(map_owned);
    std::println(2);
    if (result.has_value() != (argc == 7)) { return 5; }
    if (argc == 8 && result.error().size() != std::size_t(6)) { return 6; }
  } else if (argc == 9) {
    auto result = produce(true, 1).transform_error(map_owned).transform_error(map_again);
    std::println(2);
    if (result.has_value() || result.error() != 9 || mapper_calls != 1) { return 7; }
  } else if (argc == 10) {
    for (mut int index = 0; index < 3; index += 1) {
      auto result = produce(true, 1).transform_error(map_owned);
      std::println(2);
      if (result.has_value() || result.error().size() != std::size_t(6)) { return 8; }
    }
    if (mapper_calls != 3) { return 9; }
  } else if (argc == 11 || argc == 12) {
    auto result = propagated(argc == 12);
    std::println(2);
    if (argc == 11) {
      if (!result.has_value() || result.value().size() != std::size_t(7) ||
          mapper_calls != 0) { return 10; }
    } else if (result.has_value() || result.error().size() != std::size_t(6) ||
               mapper_calls != 1) { return 11; }
  } else if (argc <= 15) {
    auto named = produce(argc != 13, argc == 15 ? 0 : 1);
    auto result = std::move(named).transform_error(map_owned);
    std::println(2);
    if (argc == 13) {
      if (!result.has_value() || result.value().size() != std::size_t(7) ||
          mapper_calls != 0) { return 12; }
    } else if (result.has_value() || result.error().size() != std::size_t(6) ||
               mapper_calls != 1) { return 13; }
  } else if (argc <= 17) {
    auto named = perform(argc == 17);
    auto result = std::move(named).transform_error(map_owned);
    std::println(2);
    if (result.has_value() != (argc == 16) ||
        mapper_calls != (argc == 17 ? 1 : 0)) { return 14; }
  } else {
    auto result = produce(true, argc == 19 ? -1 : 1).transform_error(map_default);
    std::println(2);
    if (result.has_value() || result.error().code != 4) { return 15; }
  }
  return 0;
}
"""


def run(command: list[str], environment: dict[str, str], timeout: int = 120):
    return subprocess.run(command, text=True, capture_output=True, check=False,
                          env=environment, timeout=timeout)


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def main() -> int:
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes += [("no-elide", ["--std", "c++23", "-O0"], ["-fno-elide-constructors"]),
              ("sanitized", ["--std", "c++23", "-O1"],
               ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--mode", action="append", choices=[mode[0] for mode in modes],
                        help="Run selected modes during focused iteration; default: all.")
    parser.add_argument("--fixture", action="append", choices=("basic", "owned", "lambda", "lambda-owned", "lambda-moved"),
                        help="Run selected fixtures during focused iteration; default: all.")
    arguments = parser.parse_args()
    compiler = arguments.compiler.resolve()
    if arguments.mode:
        modes = [mode for mode in modes if mode[0] in arguments.mode]
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")

    def failure(expression):
        line = next(index + 1 for index, text in enumerate(OWNED_SOURCE.splitlines())
                    if expression in text)
        return (f"transform-error-owned.gti:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    mapping = "1\n40\n10\n"
    owned_cases = [
        (0, "2\n30\n99\n", ""),
        (0, mapping + "2\n20\n99\n", ""),
        (70, mapping + "99\n", failure("1 / error.denominator()")),
        (0, mapping + "2\n20\n99\n", ""),
        (0, mapping + "3\n20\n99\n", ""),
        (70, mapping + "20\n99\n", failure("return 12 / divisor")),
        (0, "2\n99\n", ""),
        (0, mapping + "2\n20\n99\n", ""),
        (0, mapping + "4\n20\n2\n99\n", ""),
        (0, (mapping + "2\n20\n") * 3 + "99\n", ""),
        (0, "2\n30\n99\n", ""),
        (0, mapping + "2\n20\n99\n", ""),
    ]
    owned_cases += [
        (0, "2\n30\n99\n", ""),
        (0, mapping + "2\n20\n99\n", ""),
        (70, mapping + "99\n", failure("1 / error.denominator()")),
        (0, "2\n99\n", ""),
        (0, mapping + "2\n20\n99\n", ""),
        (0, "10\n2\n50\n99\n", ""),
        (70, "10\n50\n99\n", failure("1 / (this.divisor + 1)")),
    ]
    fixtures = [("basic", SOURCE, [((), (0, "", ""))]),
                ("owned", OWNED_SOURCE,
                 [(tuple(["case"] * index), expected)
                  for index, expected in enumerate(owned_cases)])]
    lambda_source = (pathlib.Path(__file__).parent / "fixtures" /
                     "expected_lambda_mapper.gti").read_text()
    failure_line = next(i + 1 for i, line in enumerate(lambda_source.splitlines())
                        if "1 / (argc == 6" in line)
    cleanup_line = next(i + 1 for i, line in enumerate(lambda_source.splitlines())
                        if "1 / (identity - 81)" in line)
    later_line = next(i + 1 for i, line in enumerate(lambda_source.splitlines())
                     if "1 / (argc - 7)" in line)
    def lambda_secondary(primary):
        return (f"transform-error-lambda.gti:{cleanup_line}: runtime error[GTI-R0014]: "
                "failure_during_cleanup; primary [GTI-R0002] at "
                f"transform-error-lambda.gti:{primary}; secondary [GTI-R0002]\n")
    fixtures.append(("lambda", lambda_source, [
        ((), (0, "1\n80\n3\n99\n", "")),
        (("error",), (0, "1\n2\n80\n3\n99\n", "")),
        (("error", "fail"), (70, "1\n2\n80\n99\n",
            f"transform-error-lambda.gti:{failure_line}: runtime error[GTI-R0002]: "
            "division_by_zero in integer_division\n")),
        (("named", "generic", "void"), (0, "1\n1\n1\n3\n80\n99\n", "")),
        (("success", "capture", "cleanup", "failure"), (70, "1\n81\n99\n",
            f"transform-error-lambda.gti:{cleanup_line}: runtime error[GTI-R0002]: "
            "division_by_zero in integer_division\n")),
        (tuple(["mapper-secondary"] * 5), (70, "1\n2\n81\n", lambda_secondary(failure_line))),
        (tuple(["later-secondary"] * 6), (70, "1\n81\n", lambda_secondary(later_line))),
    ]))
    lambda_owned = OWNED_SOURCE.replace(
        ".transform_error(map_owned)",
        ".transform_error([](SourceError error) -> AppError { return map_owned(std::move(error)); })")
    fixtures.append(("lambda-owned", lambda_owned, [
        (tuple(["case"] * index), (status, stdout,
            stderr.replace("transform-error-owned.gti", "transform-error-lambda-owned.gti")))
        for index, (status, stdout, stderr) in enumerate(owned_cases)
    ]))
    moved_source = (pathlib.Path(__file__).parent / "fixtures" /
                    "expected_moved_lambda_mapper.gti").read_text()
    def moved_failure(function):
        lines = moved_source.splitlines()
        start = next(i for i, line in enumerate(lines) if function in line)
        line = next(i + 1 for i in range(start, len(lines))
                    if "return 1 / divisor;" in lines[i])
        return (f"transform-error-lambda-moved.gti:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
    fixtures.append(("lambda-moved", moved_source, [
        ((), (0, "1\n80\n3\n99\n", "")),
        (("error",), (0, "1\n2\n80\n3\n99\n", "")),
        (("later", "failure"), (70, "1\n2\n4\n80\n99\n", moved_failure("int later("))),
        (("later", "success", "cleanup"), (0, "1\n2\n4\n5\n80\n3\n99\n", "")),
        (("receiver", "failure", "before", "capture"),
         (70, "1\n81\n80\n99\n", moved_failure("expected<int, int> receiver_failure("))),
    ]))
    if arguments.fixture:
        fixtures = [fixture for fixture in fixtures if fixture[0] in arguments.fixture]
    with tempfile.TemporaryDirectory(prefix="gti-expected-transform-error-") as temp:
        root = pathlib.Path(temp)
        for label, flags, native in modes:
            for name, contents, cases in fixtures:
                source = root / f"transform-error-{name}.gti"
                source.write_text(contents, encoding="utf8")
                executable = root / f"transform-error-{name}-{label}"
                command = [str(compiler), str(source), *flags, "-o", str(executable)]
                if native:
                    command += ["--", *native]
                built = run(command, environment)
                if built.returncode != 0:
                    return fail(f"build {name} {label}", built)
                for program_arguments, expected in cases:
                    executed = run([str(executable), *program_arguments], environment, 30)
                    if (executed.returncode, executed.stdout, executed.stderr) != expected:
                        sys.stderr.write(f"expected: {expected!r}\n")
                        return fail(f"run {name} {label} case {len(program_arguments)}", executed)
                print(f"expected transform_error: {name} {label} passed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
