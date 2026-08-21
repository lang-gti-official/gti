#!/usr/bin/env python3

"""Prove GTI's `[[c_abi]]` layout against a real C compiler.

GTI states a record's size, alignment, and field offsets; this test asserts
those same numbers in C and lets the platform C compiler reject them. Nothing
here trusts GTI's own layout model.

Record families are data. `ADMITTED` drives both the generated C
`_Static_assert` block and the GTI `sizeof`/`alignof` check, including direct C
array fields used by GLFW's gamepad state.
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
    AdmittedRecord(
        name="ArrayHead",
        c_fields="  uint8_t a[4];\n  uint32_t b;\n",
        gti_fields="  mut uint8_t a[4];\n  mut uint32_t b;\n",
        layout=(8, 4),
        offsets=(("a", 0), ("b", 4)),
    ),
    AdmittedRecord(
        # The GLFW acceptance client's record, verbatim in shape.
        name="Gamepad",
        c_fields="  uint8_t buttons[15];\n  float axes[6];\n",
        gti_fields="  mut uint8_t buttons[15];\n  mut float axes[6];\n",
        layout=(40, 4),
        offsets=(("buttons", 0), ("axes", 16)),
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

Gamepad c_gamepad_make(void) {
  Gamepad result = {0};
  return result;
}

int32_t c_gamepad_fill(Gamepad *state) {
  state->buttons[0] = 7;
  state->buttons[14] = 9;
  state->axes[0] = 1.5f;
  state->axes[5] = -2.0f;
  return 1;
}

int32_t c_gamepad_verify(const Gamepad *state) {
  return state->buttons[1] == 4 && state->axes[1] == 3.25f;
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
  Gamepad c_gamepad_make();
  int32_t c_gamepad_fill(Gamepad* state);
  int32_t c_gamepad_verify(const Gamepad* state);
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

  mut Gamepad gamepad = c_gamepad_make();
  unsafe {
    if (c_gamepad_fill(&gamepad) != 1) {
      return 5;
    }
  }
  if (gamepad.buttons[0] != uint8_t(7) ||
      gamepad.buttons[14] != uint8_t(9) || gamepad.axes[0] != 1.5 ||
      gamepad.axes[5] != -2.0) {
    return 6;
  }
  gamepad.buttons[1] = uint8_t(4);
  gamepad.axes[1] = 3.25;
  unsafe {
    if (c_gamepad_verify(&gamepad) != 1) {
      return 7;
    }
  }

  std::println("native C record oracle passed");
  return 0;
}
"""
    )
    return "\n".join(parts)


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

    print("GTI native C record oracle passed for O0/O3 and C++20/C++23.")


if __name__ == "__main__":
    main()
