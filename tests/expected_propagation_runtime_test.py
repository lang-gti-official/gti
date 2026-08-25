#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


SOURCE = r"""
mut int scalar_state = 0;
mut int completion_state = 0;
mut int constructions = 0;
mut int destructions = 0;
mut int destruction_value_total = 0;
mut bool marker_constructed = false;
mut bool marker_alive = false;
mut bool marker_double_destroy = false;
mut int move_error_success_total = 0;

class ScopeMarker {
public:
  int value = 0;

  ScopeMarker(int initial) : value(initial) {
    marker_constructed = true;
    marker_alive = true;
  }
  ScopeMarker(ScopeMarker& other) = delete;
  ScopeMarker(ScopeMarker&& other) = default;

  ~ScopeMarker() {
    if (!marker_alive) { marker_double_destroy = true; }
    marker_alive = false;
  }
};

expected<int, int> scalar(bool fail, int error, int value) {
  if (value == 20) {
    if (scalar_state != 0) { scalar_state = 99; }
    else { scalar_state = 1; }
  } else if (value == 22) {
    if (scalar_state != 1) { scalar_state = 99; }
    else { scalar_state = 2; }
  } else {
    scalar_state = 99;
  }
  if (fail) { return unexpected(error); }
  return value;
}

expected<int, int> sum(bool first, bool second) {
  return ?scalar(first, 11, 20) + ?scalar(second, 12, 22);
}

expected<int, int> marked_value(bool fail) {
  if (fail) { return unexpected(21); }
  return 49;
}

int consume_marker(ScopeMarker marker, int value) { return value; }

expected<int, int> cleanup_prior_temporary(bool fail) {
  return consume_marker(ScopeMarker(31), ?marked_value(fail));
}

expected<void, int> complete(bool fail) {
  if (completion_state != 0) { completion_state = 99; }
  else { completion_state = 1; }
  if (fail) { return unexpected(13); }
  return;
}

expected<void, int> forward_void(bool fail) {
  ?complete(fail);
  if (completion_state != 1) { completion_state = 99; }
  else { completion_state = 2; }
  return;
}

class Payload {
public:
  mut int value = 0;

  Payload(int initial) : value(initial) { constructions += 1; }
  Payload(Payload& other) = delete;
  Payload(Payload&& other) = default;

  ~Payload() {
    destructions += 1;
    destruction_value_total += value;
  }
};

expected<Payload, int> make_payload(bool fail) {
  if (fail) { return unexpected(17); }
  return Payload(41);
}

expected<Payload, int> forward_payload(bool fail) {
  mut Payload payload = ?make_payload(fail);
  payload.value += 1;
  return std::move(payload);
}

expected<Payload, int> forward_named_payload(bool fail) {
  mut expected<Payload, int> result = make_payload(fail);
  mut Payload payload = ?std::move(result);
  payload.value += 2;
  return std::move(payload);
}

class CopyPayload {
public:
  int value = 0;

  CopyPayload(int initial) : value(initial) {}
  CopyPayload(CopyPayload& other) = default;
  CopyPayload(CopyPayload&& other) = default;

  int read() { return this.value; }
};

expected<CopyPayload, int> make_copy_payload(bool fail) {
  if (fail) { return unexpected(19); }
  return CopyPayload(43);
}

expected<CopyPayload, int> forward_named_copy_payload(bool fail) {
  expected<CopyPayload, int> result = make_copy_payload(fail);
  return ?result;
}

class CopyError {
public:
  int code = 0;

  CopyError(int initial) : code(initial) {}
  CopyError(CopyError& other) = default;
  CopyError(CopyError&& other) = default;

  int read() { return this.code; }
};

expected<int, CopyError> make_copy_error(bool fail) {
  if (fail) { return unexpected(CopyError(23)); }
  return 47;
}

expected<int, CopyError> forward_named_copy_error(bool fail) {
  expected<int, CopyError> result = make_copy_error(fail);
  return ?result;
}

class MoveError {
public:
  int code = 0;

  MoveError(int initial) : code(initial) {}
  MoveError(MoveError& other) = delete;
  MoveError(MoveError&& other) = default;

  int read() { return this.code; }
};

expected<int, MoveError> make_move_error(bool fail, int code, int value) {
  if (fail) { return unexpected(MoveError(code)); }
  return value;
}

expected<int, MoveError> forward_prvalue_move_error(bool fail) {
  int value = ?make_move_error(fail, 29, 53);
  move_error_success_total += value;
  return value;
}

expected<int, MoveError> forward_named_move_error(bool fail) {
  mut auto result = make_move_error(fail, 31, 59);
  int value = ?std::move(result);
  move_error_success_total += value;
  return value;
}

int main() {
  expected<int, int> success = sum(false, false);
  if (!success.has_value() || success.value() != 42 || scalar_state != 2) {
    return 1;
  }

  scalar_state = 0;
  expected<int, int> first_failure = sum(true, false);
  if (first_failure.has_value() || first_failure.error() != 11 ||
      scalar_state != 1) {
    return 2;
  }

  scalar_state = 0;
  expected<int, int> second_failure = sum(false, true);
  if (second_failure.has_value() || second_failure.error() != 12 ||
      scalar_state != 2) {
    return 3;
  }

  expected<void, int> void_success = forward_void(false);
  if (!void_success.has_value() || completion_state != 2) { return 4; }
  completion_state = 0;
  expected<void, int> void_failure = forward_void(true);
  if (void_failure.has_value() || void_failure.error() != 13 ||
      completion_state != 1) {
    return 5;
  }

  {
    expected<Payload, int> owning_success = forward_payload(false);
    if (!owning_success.has_value()) { return 6; }
  }
  if (destruction_value_total != 42 || constructions != destructions) {
    return 7;
  }

  {
    expected<Payload, int> owning_failure = forward_payload(true);
    if (owning_failure.has_value() || owning_failure.error() != 17) {
      return 8;
    }
  }
  if (destruction_value_total != 42 || constructions != destructions) {
    return 9;
  }

  {
    expected<Payload, int> named_success = forward_named_payload(false);
    if (!named_success.has_value()) { return 10; }
  }
  if (destruction_value_total != 85 || constructions != destructions) {
    return 11;
  }

  {
    expected<Payload, int> named_failure = forward_named_payload(true);
    if (named_failure.has_value() || named_failure.error() != 17) {
      return 12;
    }
  }
  if (destruction_value_total != 85 || constructions != destructions) {
    return 13;
  }

  expected<CopyPayload, int> copied_success =
      forward_named_copy_payload(false);
  if (!copied_success.has_value() || copied_success.value().read() != 43) {
    return 14;
  }

  expected<CopyPayload, int> copied_failure =
      forward_named_copy_payload(true);
  if (copied_failure.has_value() || copied_failure.error() != 19) {
    return 15;
  }

  expected<int, CopyError> copied_error_success =
      forward_named_copy_error(false);
  if (!copied_error_success.has_value() ||
      copied_error_success.value() != 47) {
    return 16;
  }

  expected<int, CopyError> copied_error_failure =
      forward_named_copy_error(true);
  if (copied_error_failure.has_value() ||
      copied_error_failure.error().read() != 23) {
    return 17;
  }

  [[discard]] forward_prvalue_move_error(false);
  [[discard]] forward_prvalue_move_error(true);
  [[discard]] forward_named_move_error(false);
  [[discard]] forward_named_move_error(true);
  if (move_error_success_total != 112) {
    return 18;
  }

  expected<int, int> cleaned_success = cleanup_prior_temporary(false);
  if (!cleaned_success.has_value() || cleaned_success.value() != 49 ||
      !marker_constructed || marker_alive || marker_double_destroy) {
    return 19;
  }

  marker_constructed = false;
  marker_alive = false;
  marker_double_destroy = false;
  expected<int, int> cleaned_failure = cleanup_prior_temporary(true);
  if (cleaned_failure.has_value() || cleaned_failure.error() != 21 ||
      !marker_constructed || marker_alive || marker_double_destroy) {
    return 20;
  }
  return 0;
}
"""


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: expected_propagation_runtime_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-expected-propagation-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "expected-propagation.gti"
        source.write_text(SOURCE, encoding="utf8")
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                executable = root / f"propagation-{standard}-{optimization}"
                built = run(
                    [
                        str(compiler),
                        str(source),
                        "--std",
                        standard,
                        f"-{optimization}",
                        "-o",
                        str(executable),
                    ]
                )
                if built.returncode != 0:
                    return fail(f"build {standard} {optimization}", built)

                executed = run([str(executable)])
                if (
                    executed.returncode != 0
                    or executed.stdout != ""
                    or executed.stderr != ""
                ):
                    return fail(f"run {standard} {optimization}", executed)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
