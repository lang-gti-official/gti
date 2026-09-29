#!/usr/bin/env python3

"""Generate GTI's typed diagnostic registry from its checked source data."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys


PLACEHOLDER = re.compile(r"\{(\d+)\}")
PHASES = {"Lexing", "SourceLoading", "Parsing", "Semantics", "Driver", "Backend"}
SEVERITIES = {"Error", "Warning", "Information", "Hint"}


def quote(value: str) -> str:
    return json.dumps(value)


def validate(entries: list[dict[str, object]]) -> None:
    seen_ids: set[str] = set()
    seen_codes: set[str] = set()
    for entry in entries:
        identifier = entry.get("id")
        code = entry.get("code")
        phase = entry.get("phase")
        severity = entry.get("severity")
        group = entry.get("group")
        message = entry.get("format")
        arguments = entry.get("arguments")
        if not isinstance(identifier, str) or not re.fullmatch(r"[A-Za-z][A-Za-z0-9]*", identifier):
            raise ValueError(f"invalid diagnostic id: {identifier!r}")
        if not isinstance(code, str) or not re.fullmatch(r"GTI-[A-Z][0-9]{4}", code):
            raise ValueError(f"invalid diagnostic code for {identifier}: {code!r}")
        if identifier in seen_ids or code in seen_codes:
            raise ValueError(f"duplicate diagnostic id or code: {identifier} / {code}")
        if phase not in PHASES:
            raise ValueError(f"invalid phase for {identifier}: {phase!r}")
        if severity not in SEVERITIES:
            raise ValueError(f"invalid severity for {identifier}: {severity!r}")
        if not isinstance(group, str) or not group:
            raise ValueError(f"diagnostic {identifier} requires a non-empty group")
        if not isinstance(message, str) or not isinstance(arguments, int) or arguments < 0:
            raise ValueError(f"invalid format contract for {identifier}")
        placeholders = {int(match.group(1)) for match in PLACEHOLDER.finditer(message)}
        expected = set(range(arguments))
        if placeholders != expected:
            raise ValueError(
                f"format placeholders for {identifier} are {sorted(placeholders)}, expected {sorted(expected)}"
            )
        seen_ids.add(identifier)
        seen_codes.add(code)


def render(entries: list[dict[str, object]], source: pathlib.Path) -> str:
    source_label = "/".join(source.parts[-2:])
    enum_rows = "\n".join(f"  {entry['id']}," for entry in entries)
    descriptor_rows = "\n".join(
        "  DiagnosticDescriptor{DiagnosticId::%s, %s, DiagnosticPhase::%s, "
        "DiagnosticSeverity::%s, %s, %s, %s},"
        % (
            entry["id"],
            quote(entry["code"]),
            entry["phase"],
            entry["severity"],
            quote(entry["group"]),
            quote(entry["format"]),
            entry["arguments"],
        )
        for entry in entries
    )
    return f"""// Generated from {source_label}; do not edit.
#pragma once

#include \"gti/diagnostic.h\"

#include <array>
#include <cstddef>
#include <string_view>

namespace lang {{

enum class DiagnosticId {{
{enum_rows}
}};

struct DiagnosticDescriptor {{
  DiagnosticId id;
  std::string_view code;
  DiagnosticPhase phase;
  DiagnosticSeverity severity;
  std::string_view group;
  std::string_view messageFormat;
  std::size_t argumentCount;
}};

inline constexpr std::array<DiagnosticDescriptor, {len(entries)}> diagnosticDescriptors{{{{
{descriptor_rows}
}}}};

[[nodiscard]] constexpr const DiagnosticDescriptor &
diagnosticDescriptor(DiagnosticId id) {{
  return diagnosticDescriptors[static_cast<std::size_t>(id)];
}}

}} // namespace lang
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    try:
        data = json.loads(args.input.read_text(encoding="utf-8"))
        if not isinstance(data, list):
            raise ValueError("diagnostic registry must be a JSON array")
        validate(data)
        rendered = render(data, args.input)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        if not args.output.exists() or args.output.read_text(encoding="utf-8") != rendered:
            args.output.write_text(rendered, encoding="utf-8")
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"generate_diagnostics.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
