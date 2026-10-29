#!/usr/bin/env python3
"""Checked integer arithmetic matches exact math with and without builtins.

Ordinary operators on 8- and 16-bit operands promote to int32, so their
overflow domain is int32; the checked and saturating functions keep the
operand width. The generated program reads its operands from program
arguments, so neither
GTI nor the native compiler can fold the operations under test. Every build
mode (native overflow builtins and the portable GTI_PORTABLE_OVERFLOW_CHECKS
path, at O0 and O3) must agree with an exact Python oracle. The checked and
saturating operations cover every ordered boundary pair in one process; the
ordinary operators run once per case because an overflow terminates.
"""

import pathlib
import subprocess
import sys
import tempfile

TYPES = {
    "int8": ("int8_t", 8, True),
    "int16": ("int16_t", 16, True),
    "int32": ("int32_t", 32, True),
    "int64": ("int64_t", 64, True),
    "uint8": ("uint8_t", 8, False),
    "uint16": ("uint16_t", 16, False),
    "uint32": ("uint32_t", 32, False),
    "uint64": ("uint64_t", 64, False),
}


def bounds(bits: int, signed: bool) -> tuple[int, int]:
    return (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else (0, (1 << bits) - 1)


def boundary_values(bits: int, signed: bool) -> list[int]:
    low, high = bounds(bits, signed)
    root = 1 << (bits // 2 - (1 if signed else 0))
    candidates = {low, low + 1, 0, 1, 2, 3, high - 1, high, high // 2, high // 2 + 1,
                  root - 1, root, root + 1}
    if signed:
        candidates |= {-1, -2, -3, low // 2, low // 2 - 1, -root, -root - 1, -root + 1}
    return sorted(value for value in candidates if low <= value <= high)


def program() -> str:
    lines = [
        "import std.charconv;",
        "import std.numeric;",
        "import std.panic;",
        "import std.string;",
        "import std.vector;",
        "",
    ]
    for name, (spelling, _, _) in TYPES.items():
        lines += [
            f"{spelling} parse_{name}(std::string& text) {{",
            f"  mut {spelling} value = {spelling}(0);",
            "  auto parsed = std::from_chars(text.view(), value);",
            "  if (parsed.ec != std::parse_errc::none || parsed.position != text.size()) {",
            '    std::panic("unparsable operand");',
            "  }",
            "  return value;",
            "}",
            f"void print_{name}(expected<{spelling}, std::arithmetic_errc> result) {{",
            "  if (result.has_value()) { std::println(result.value()); }",
            '  else { std::println("overflow"); }',
            "}",
            f"void sweep_{name}(std::vector<std::string>& argv) {{",
            f"  mut std::vector<{spelling}> values = std::vector<{spelling}>();",
            "  for (mut std::size_t index = std::size_t(3); index < argv.size(); index++) {",
            f"    values.push_back(parse_{name}(argv[index]));",
            "  }",
            "  for (auto& left : values) {",
            "    for (auto& right : values) {",
            f"      print_{name}(std::checked_add(left, right));",
            f"      print_{name}(std::checked_sub(left, right));",
            f"      print_{name}(std::checked_mul(left, right));",
            "      std::println(std::saturating_add(left, right));",
            "      std::println(std::saturating_sub(left, right));",
            "      std::println(std::saturating_mul(left, right));",
            "    }",
            "  }",
            "}",
            f"void operate_{name}(std::vector<std::string>& argv) {{",
            f"  {spelling} left = parse_{name}(argv[std::size_t(3)]);",
            f"  {spelling} right = parse_{name}(argv[std::size_t(4)]);",
            '  if (argv[std::size_t(2)] == "add") { std::println(left + right); }',
            '  else if (argv[std::size_t(2)] == "sub") { std::println(left - right); }',
            '  else if (argv[std::size_t(2)] == "mul") { std::println(left * right); }',
            '  else if (argv[std::size_t(2)] == "add-assign") {',
            "    mut auto target = left;",
            "    target += right;",
            "    std::println(target);",
            "  }",
            '  else { std::panic("unknown operation"); }',
            "}",
            "",
        ]
    lines += ["int main(int argc, std::vector<std::string> argv) {",
              "  if (argc < 3) { return 2; }",
              "  std::string& type = argv[std::size_t(1)];",
              '  bool sweep = argv[std::size_t(2)] == "sweep";']
    for name in TYPES:
        lines += [f'  if (type == "{name}") {{',
                  f"    if (sweep) {{ sweep_{name}(argv); }} else {{ operate_{name}(argv); }}",
                  "    return 0;",
                  "  }"]
    lines += ["  return 3;", "}", ""]
    return "\n".join(lines)


def expected_sweep(bits: int, signed: bool, values: list[int]) -> str:
    low, high = bounds(bits, signed)

    def checked(value: int) -> str:
        return str(value) if low <= value <= high else "overflow"

    def saturate(value: int) -> str:
        return str(min(max(value, low), high))

    out = []
    for left in values:
        for right in values:
            for value in (left + right, left - right, left * right):
                out.append(checked(value))
            for value in (left + right, left - right, left * right):
                out.append(saturate(value))
    return "\n".join(out) + "\n"


def operator_cases(bits: int, signed: bool) -> list[tuple[str, int, int, int]]:
    low, high = bounds(bits, signed)
    root = 1 << (bits // 2 - (1 if signed else 0))
    cases = [
        ("add", high - 1, 1), ("add", high, 1), ("add", high, 0),
        ("sub", low + 1, 1), ("sub", low, 1), ("sub", 0, 0),
        ("mul", root - 1, root - 1), ("mul", root, root), ("mul", high, 2),
        ("mul", high, 1), ("mul", 0, high),
    ]
    if signed:
        cases += [("add", low, -1), ("sub", high, -1), ("mul", low, -1),
                  ("mul", -1, low), ("mul", low, 1), ("mul", -root, root)]
    if bits >= 32:
        # Compound assignment shares the addition contract; for narrower types
        # it promotes and then reports the conversion instead.
        cases += [("add-assign", high, 1), ("add-assign", high - 1, 1)]
    results = []
    for operation, left, right in cases:
        exact = {"add": left + right, "sub": left - right, "mul": left * right,
                 "add-assign": left + right}[operation]
        results.append((operation, left, right, exact))
    return results


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False, timeout=120)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: checked_integer_overflow_runtime_test.py <gti>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    failures = 0
    executions = 0
    with tempfile.TemporaryDirectory(prefix="gti-checked-overflow-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "checked_overflow.gti"
        source.write_text(program(), encoding="utf8")
        for optimization in ("O0", "O3"):
            for portable in (False, True):
                mode = f"{optimization}-{'portable' if portable else 'builtin'}"
                executable = root / f"checked-{mode}"
                command = [str(compiler), str(source), f"-{optimization}", "-o", str(executable)]
                if portable:
                    command += ["--", "-DGTI_PORTABLE_OVERFLOW_CHECKS"]
                built = run(command)
                if built.returncode != 0:
                    sys.stderr.write(f"{mode}: build failed\n{built.stdout}{built.stderr}")
                    return 1
                for name, (_, bits, signed) in TYPES.items():
                    values = boundary_values(bits, signed)
                    swept = run([str(executable), name, "sweep", *map(str, values)])
                    executions += 1
                    if swept.returncode != 0 or swept.stdout != expected_sweep(bits, signed, values):
                        sys.stderr.write(f"{mode} {name} sweep mismatch (exit {swept.returncode})\n")
                        failures += 1
                    low, high = bounds(bits, signed) if bits >= 32 else bounds(32, True)
                    for operation, left, right, exact in operator_cases(bits, signed):
                        result = run([str(executable), name, operation, str(left), str(right)])
                        executions += 1
                        if low <= exact <= high:
                            ok = result.returncode == 0 and result.stdout == f"{exact}\n"
                        else:
                            spelled = {"add": "addition", "sub": "subtraction",
                                       "mul": "multiplication",
                                       "add-assign": "addition"}[operation]
                            ok = (result.returncode == 70 and result.stdout == "" and
                                  f"integer_overflow in {spelled}" in result.stderr)
                        if not ok:
                            sys.stderr.write(
                                f"{mode} {name} {operation} {left} {right}: exit "
                                f"{result.returncode} stdout {result.stdout!r} "
                                f"stderr {result.stderr.strip()!r}\n")
                            failures += 1
    if failures:
        sys.stderr.write(f"{failures} checked-overflow mismatches\n")
        return 1
    print(f"{executions} checked-overflow executions matched exact arithmetic "
          "under builtin and portable checks at O0 and O3")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
