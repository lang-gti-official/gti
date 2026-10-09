#!/usr/bin/env python3
"""std::format output stays byte-identical across a broad spec matrix.

The program is generated deterministically. It covers integers of every width
with sign, base, alternate, zero-padding, fill and alignment specs; char and
`c` presentations; strings and views with precision; bool; float and double
presentations; positional and escaped fields; long output; `try_format_to`;
and rejected specs. The expected output was produced by the formatter before
its allocation-lean rewrite, so any change to formatting results fails here.
Statements are grouped into small functions to keep each body small.
"""

import pathlib
import subprocess
import sys
import tempfile

INTEGERS = {
    "int8_t": ["-128", "-1", "0", "7", "127"],
    "int16_t": ["-32768", "-42", "0", "255", "32767"],
    "int32_t": ["-2147483648", "-12345", "0", "65", "2147483647"],
    "int64_t": ["-9223372036854775808", "-1", "0", "4096", "9223372036854775807"],
    "uint8_t": ["0", "65", "255"],
    "uint16_t": ["0", "10", "65535"],
    "uint32_t": ["0", "48879", "4294967295"],
    "uint64_t": ["0", "3", "18446744073709551615"],
}
INTEGER_SPECS = ["", ":5", ":<6", ":^7", ":>8", ":05", ":+", ": ", ":x", ":#x", ":#X",
                 ":o", ":#o", ":b", ":#b", ":#B", ":*^9", ":+08x", ":-^11", ":#010b",
                 ":_<4", ":=>12", ":#12o", ":X", ":d", ":+d", ": 6", ":0"]
CHAR_SPECS = ["", ":5", ":^5", ":>3", ":c", ":x", ":d", ":#x", ":*<4", ":.0", ":.1"]
TEXT_SPECS = ["", ":8", ":<8", ":^9", ":>10", ":.2", ":>8.3", ":*^12.5", ":.0", ":3"]
FLOAT_SPECS = ["", ":.3f", ":e", ":10.2f", ":<12g", ":+.1e", ":08.3f", ":^14.4E", ":G"]


def program() -> str:
    lines = ["#include <std/format>", "#include <std/string>",
             "void show(expected<std::string, std::format_errc> result) {\n"
             "  if (result.has_value()) { std::println(result.value().view()); }\n"
             "  else { std::println(\"error\"); }\n}", "int main() {"]
    for spelling, values in INTEGERS.items():
        for value in values:
            for spec in INTEGER_SPECS:
                lines.append(f'  show(std::try_format("[{{{spec}}}]", {spelling}({value})));')
            if not value.startswith("-") and 32 <= int(value) <= 126:
                lines.append(f'  show(std::try_format("[{{:c}}]", {spelling}({value})));')
    for character in ["'a'", "'Z'", "'0'", "' '"]:
        for spec in CHAR_SPECS:
            lines.append(f'  show(std::try_format("[{{{spec}}}]", {character}));')
    for text in ['"gti"', '""', '"formatting"']:
        for spec in TEXT_SPECS:
            lines.append(f'  show(std::try_format("[{{{spec}}}]", {text}));')
            lines.append(f'  show(std::try_format("[{{{spec}}}]", std::string({text})));')
    for boolean in ["true", "false"]:
        for spec in ["", ":6", ":^7", ":>8", ":.2"]:
            lines.append(f'  show(std::try_format("[{{{spec}}}]", {boolean}));')
    for value in ["0.0", "-1.5", "3.14159265", "0.0000001", "12345678.9"]:
        for spec in FLOAT_SPECS:
            lines.append(f'  show(std::try_format("[{{{spec}}}]", double({value})));')
            lines.append(f'  show(std::try_format("[{{{spec}}}]", float({value})));')
    # Mixed patterns, literals, escapes, positional fields and long output
    # share locals, so they form one group of their own.
    mixed = [
        '  show(std::try_format("{}:{}|{{}}|{:>4}", int32_t(1), int64_t(-2), uint8_t(3)));',
        '  show(std::try_format("{1}-{0}-{1}", int32_t(10), "x"));',
        '  show(std::try_format("{:#x}{:#o}{:#b}", uint32_t(255), uint32_t(8), uint32_t(5)));',
        '  show(std::try_format("{{{}}}", int32_t(9)));',
        '  mut std::string long_text = std::string();',
        '  for (mut int32_t index = 0; index < 200; index++) {',
        '    long_text.append(std::format("{:04}|", index).view());',
        '  }',
        '  std::println(long_text.view());',
        '  mut std::string target = std::string("prefix:");',
        '  [[discard]] std::try_format_to(target, "{}+{}", int32_t(4), int32_t(5));',
        '  std::println(target.view());',
        '  std::println(std::format("{}", std::try_format("{:q}", int32_t(1)).has_value()));',
        '  std::println(std::format("{}", std::try_format("{} {}", int32_t(1)).has_value()));',
    ]
    lines += ["  return 0;", "}", ""]
    # Keep every body small: one function per group of ten statements.
    header, body = lines[:4], lines[4:]
    tail_start = body.index("  return 0;")
    statements, tail = body[:tail_start], body[tail_start:]
    groups = []
    current = []
    depth = 0
    for statement in statements:
        current.append(statement)
        depth += statement.count("{") - statement.count("}")
        if depth == 0 and len(current) >= 10:
            groups.append(current)
            current = []
    if current:
        groups.append(current)
    groups.append(mixed)
    out = header[:3]
    for index, group in enumerate(groups):
        out.append(f"void group_{index}() {{")
        out.extend(group)
        out.append("}")
    out.append("int main() {")
    out.extend(f"  group_{index}();" for index in range(len(groups)))
    out.extend(tail)
    return "\n".join(out) + "\n"



def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: format_output_matrix_runtime_test.py <gti> <expected>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    expected = pathlib.Path(sys.argv[2]).read_text(encoding="utf8")
    with tempfile.TemporaryDirectory(prefix="gti-format-matrix-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "format_matrix.gti"
        source.write_text(program(), encoding="utf8")
        for level in ("O0", "O3"):
            executable = root / f"format-matrix-{level}"
            built = subprocess.run([str(compiler), str(source), f"-{level}", "-o", str(executable)],
                                   text=True, capture_output=True, timeout=600)
            if built.returncode != 0:
                sys.stderr.write(built.stdout + built.stderr)
                return 1
            ran = subprocess.run([str(executable)], text=True, capture_output=True, timeout=120)
            if ran.returncode != 0 or ran.stdout != expected:
                got = ran.stdout.splitlines()
                want = expected.splitlines()
                for index, (left, right) in enumerate(zip(want, got)):
                    if left != right:
                        sys.stderr.write(f"{level}: line {index + 1}: expected {left!r}, got {right!r}\n")
                        break
                sys.stderr.write(f"{level}: exit {ran.returncode}, {len(got)} of {len(want)} lines\n")
                return 1
    print(f"{len(expected.splitlines())} formatted lines matched at O0 and O3")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
