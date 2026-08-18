#!/usr/bin/env python3

"""Prove GTI's `[[c_abi]]` layout against a real C compiler.

GTI states a record's size, alignment, and field offsets; this test asserts
those same numbers in C and lets the platform C compiler reject them. Nothing
here trusts GTI's own layout model.

Record families are data. `ADMITTED` holds the families the compiler accepts
today and drives both the generated C `_Static_assert` block and the GTI
`sizeof`/`alignof` check. `PENDING` holds families that `S-FFI-02` family F1
(fixed-array `[[c_abi]]` fields) will admit; because F1 is not implemented,
each pending case asserts the *current* rejection and its exact diagnostic.

HOW A PENDING CASE FLIPS WHEN F1 LANDS
--------------------------------------
1. Move the entry from `PENDING` to `ADMITTED`.
2. Rename `intended_layout` to `layout` and `intended_offsets` to `offsets`;
   the values are already the real C ones, measured with `cc`, so they do not
   change.
3. Delete `rejected_field` and `diagnostic`.
Nothing else moves: the generated C block, the static asserts, and the GTI
check all derive from the same fields either way. A pending case that starts
compiling before step 1 fails loudly rather than silently passing.
"""

from dataclasses import dataclass
import os
from pathlib import Path
import subprocess
import sys
import tempfile


@dataclass(frozen=True)
class AdmittedRecord:
    """A `[[c_abi]]` record family the compiler admits today."""

    name: str
    c_fields: str
    gti_fields: str
    layout: tuple[int, int]
    offsets: tuple[tuple[str, int], ...]


@dataclass(frozen=True)
class PendingRecord:
    """A record family F1 will admit. See "HOW A PENDING CASE FLIPS".

    `intended_layout` and `intended_offsets` are the real layout measured with
    the platform C compiler, so the target is recorded before the compiler can
    produce it. `rejected_field` is the first field the current admission check
    reports, which is the one the diagnostic names.
    """

    name: str
    c_fields: str
    gti_fields: str
    rejected_field: str
    diagnostic: str
    intended_layout: tuple[int, int]
    intended_offsets: tuple[tuple[str, int], ...]


ADMITTED: tuple[AdmittedRecord, ...] = (
    AdmittedRecord(
        name="Vec2",
        c_fields="  float x;\n  float y;\n",
        gti_fields="  mut float x;\n  mut float y;\n",
        layout=(8, 4),
        offsets=(("x", 0), ("y", 4)),
    ),
    AdmittedRecord(
        name="Packet",
        c_fields=(
            "  uint8_t tag;\n"
            "  uint64_t serial;\n"
            "  uint16_t flags;\n"
            "  Vec2 point;\n"
        ),
        gti_fields=(
            "  mut uint8_t tag;\n"
            "  mut uint64_t serial;\n"
            "  mut uint16_t flags;\n"
            "  mut Vec2 point;\n"
        ),
        layout=(32, 8),
        offsets=(("tag", 0), ("serial", 8), ("flags", 16), ("point", 20)),
    ),
    AdmittedRecord(
        name="Link",
        c_fields="  Packet *next;\n  uint32_t code;\n",
        gti_fields="  mut Packet* next;\n  mut uint32_t code;\n",
        layout=(16, 8),
        offsets=(("next", 0), ("code", 8)),
    ),
)


# Layouts measured with `cc -std=c17` on the reference target and confirmed by
# the generated static asserts once F1 admits them.
PENDING: tuple[PendingRecord, ...] = (
    PendingRecord(
        name="ArrayHead",
        c_fields="  uint8_t a[4];\n  uint32_t b;\n",
        gti_fields="  mut uint8_t a[4];\n  mut uint32_t b;\n",
        rejected_field="a",
        diagnostic="GTI-S2064",
        intended_layout=(8, 4),
        intended_offsets=(("a", 0), ("b", 4)),
    ),
    PendingRecord(
        # The GLFW acceptance client's blocked record, verbatim in shape.
        name="Gamepad",
        c_fields="  uint8_t buttons[15];\n  float axes[6];\n",
        gti_fields="  mut uint8_t buttons[15];\n  mut float axes[6];\n",
        rejected_field="buttons",
        diagnostic="GTI-S2064",
        intended_layout=(40, 4),
        intended_offsets=(("buttons", 0), ("axes", 16)),
    ),
)


def run(command, *, cwd=None, check=True):
    completed = subprocess.run(
        [str(part) for part in command],
        cwd=cwd,
        capture_output=True,
        text=True,
        check=False,
    )
    if check and completed.returncode != 0:
        raise RuntimeError(
            f"command failed ({completed.returncode}): {' '.join(map(str, command))}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return completed


def c_record(name: str, fields: str) -> str:
    return f"typedef struct {name} {{\n{fields}}} {name};\n"


def c_asserts(name: str, layout: tuple[int, int],
              offsets: tuple[tuple[str, int], ...]) -> str:
    size, alignment = layout
    lines = [
        f'_Static_assert(sizeof({name}) == {size}, "{name} size");',
        f'_Static_assert(_Alignof({name}) == {alignment}, "{name} alignment");',
    ]
    lines.extend(
        f'_Static_assert(offsetof({name}, {field}) == {offset}, '
        f'"{name}.{field} offset");'
        for field, offset in offsets
    )
    return "\n".join(lines) + "\n"


def gti_record(name: str, fields: str) -> str:
    return f"[[c_abi]]\nstruct {name} {{\n{fields}}};\n"


def gti_layout_check(records) -> str:
    terms = []
    for record in records:
        size, alignment = record.layout
        terms.append(f"sizeof({record.name}) != uint64_t({size})")
        terms.append(f"alignof({record.name}) != uint64_t({alignment})")
    return " ||\n      ".join(terms)


# Cross-boundary behaviour: by-value passage, return, and pointer mutation.
# These are not record cases and stay literal.
C_FUNCTIONS = r"""
Packet c_packet_make(uint8_t tag, uint64_t serial, uint16_t flags,
                     float x, float y) {
  Packet result = {
      .tag = tag,
      .serial = serial,
      .flags = flags,
      .point = {.x = x, .y = y},
  };
  return result;
}

Packet c_packet_roundtrip(Packet value) { return value; }

uint64_t c_packet_checksum(Packet value) {
  return (uint64_t)value.tag + value.serial + (uint64_t)value.flags;
}

void c_packet_translate(Packet *value, float dx, float dy) {
  value->point.x += dx;
  value->point.y += dy;
}

Link c_link_null(uint32_t code) {
  Link result = {.next = NULL, .code = code};
  return result;
}
"""

GTI_BEHAVIOUR = r"""
extern "C" {
  Packet c_packet_make(uint8_t tag, uint64_t serial, uint16_t flags,
                       float x, float y);
  Packet c_packet_roundtrip(Packet value);
  uint64_t c_packet_checksum(Packet value);
  void c_packet_translate(Packet* value, float dx, float dy);
  Link c_link_null(uint32_t code);
}

Link make_link(uint32_t code) {
  unsafe {
    return c_link_null(code);
  }
}
"""


def build_c_source() -> str:
    parts = ["#include <stddef.h>\n#include <stdint.h>\n"]
    parts.extend(c_record(r.name, r.c_fields) for r in ADMITTED)
    parts.extend(c_asserts(r.name, r.layout, r.offsets) for r in ADMITTED)
    parts.append(C_FUNCTIONS)
    return "\n".join(parts)


def build_gti_source() -> str:
    parts = [gti_record(r.name, r.gti_fields) for r in ADMITTED]
    parts.append(GTI_BEHAVIOUR)
    parts.append(
        "int main() {\n"
        f"  if ({gti_layout_check(ADMITTED)}) {{\n"
        "    return 1;\n"
        "  }\n"
        + r"""
  mut Packet packet = c_packet_make(
      uint8_t(7), uint64_t(100), uint16_t(9), 1.5, 2.0);
  packet = c_packet_roundtrip(packet);
  if (packet.tag != uint8_t(7) || packet.serial != uint64_t(100) ||
      packet.flags != uint16_t(9) || packet.point.x != 1.5 ||
      packet.point.y != 2.0 ||
      c_packet_checksum(packet) != uint64_t(116)) {
    return 2;
  }

  unsafe {
    c_packet_translate(&packet, 0.5, 1.0);
  }
  if (packet.point.x != 2.0 || packet.point.y != 3.0) {
    return 3;
  }

  mut Link link = make_link(uint32_t(77));
  if (link.code != uint32_t(77)) {
    return 4;
  }

  std::println("native C record oracle passed");
  return 0;
}
"""
    )
    return "\n".join(parts)


def check_pending(gti: Path, root: Path, temp: Path) -> None:
    """Assert each F1 family is still rejected, with its exact diagnostic.

    This is the half of the oracle that runs before the feature exists. It
    pins the diagnostic and the field it names, so F1 landing is visible here
    as a failure rather than as silence.
    """
    for record in PENDING:
        source = temp / f"pending_{record.name}.gti"
        source.write_text(
            gti_record(record.name, record.gti_fields) + "int main() { return 0; }\n",
            encoding="utf-8",
        )
        result = run(
            [gti, source, "--emit-cpp", "-o", temp / f"pending_{record.name}.cpp"],
            cwd=root,
            check=False,
        )
        if result.returncode == 0:
            raise RuntimeError(
                f"{record.name} now compiles as a [[c_abi]] record. S-FFI-02 "
                f"family F1 has landed: move this case from PENDING to "
                f"ADMITTED per the flip procedure at the top of this file. Its "
                f"intended layout {record.intended_layout} and offsets "
                f"{record.intended_offsets} become the asserted ones."
            )
        if record.diagnostic not in result.stderr:
            raise RuntimeError(
                f"{record.name} was rejected without {record.diagnostic}:\n"
                f"{result.stderr}"
            )
        if f"'{record.rejected_field}'" not in result.stderr:
            raise RuntimeError(
                f"{record.diagnostic} for {record.name} did not name field "
                f"'{record.rejected_field}':\n{result.stderr}"
            )
    print(
        f"F1 pending: {len(PENDING)} fixed-array families still rejected with "
        "their exact diagnostics."
    )


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: native_record_c_oracle_test.py <gti> <repo-root>")

    gti = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve()
    compiler = os.environ.get("CC", "cc")

    with tempfile.TemporaryDirectory(prefix="gti-native-record-") as temp_dir:
        temp = Path(temp_dir)
        c_path = temp / "native_record_oracle.c"
        object_path = temp / "native_record_oracle.o"
        source_path = temp / "native_record_oracle.gti"
        c_path.write_text(build_c_source(), encoding="utf-8")
        source_path.write_text(build_gti_source(), encoding="utf-8")

        run([compiler, "-std=c17", "-O2", "-c", c_path, "-o", object_path])

        for optimization in ("-O0", "-O3"):
            for standard in ("c++20", "c++23"):
                suffix = ".exe" if os.name == "nt" else ""
                binary = temp / f"native-record-{optimization[2:]}-{standard}{suffix}"
                run(
                    [
                        gti,
                        source_path,
                        optimization,
                        "--std",
                        standard,
                        "-o",
                        binary,
                        "--",
                        object_path,
                    ],
                    cwd=root,
                )
                executed = run([binary])
                if executed.stdout != "native C record oracle passed\n":
                    raise RuntimeError(
                        f"unexpected native oracle output for {optimization}/{standard}: "
                        f"{executed.stdout!r}"
                    )

        check_pending(gti, root, temp)

    print("GTI native C record oracle passed for O0/O3 and C++20/C++23.")


if __name__ == "__main__":
    main()
