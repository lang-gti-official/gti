#!/usr/bin/env python3

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run(command, *, cwd=None):
    completed = subprocess.run(
        [str(part) for part in command],
        cwd=cwd,
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"command failed ({completed.returncode}): {' '.join(map(str, command))}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return completed


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: native_record_c_oracle_test.py <gti> <repo-root>")

    gti = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve()
    compiler = os.environ.get("CC", "cc")

    c_source = r"""
#include <stddef.h>
#include <stdint.h>

typedef struct Vec2 {
  float x;
  float y;
} Vec2;

typedef struct Packet {
  uint8_t tag;
  uint64_t serial;
  uint16_t flags;
  Vec2 point;
} Packet;

typedef struct Link {
  Packet *next;
  uint32_t code;
} Link;

_Static_assert(sizeof(Vec2) == 8, "Vec2 size");
_Static_assert(_Alignof(Vec2) == 4, "Vec2 alignment");
_Static_assert(offsetof(Vec2, x) == 0, "Vec2.x offset");
_Static_assert(offsetof(Vec2, y) == 4, "Vec2.y offset");
_Static_assert(sizeof(Packet) == 32, "Packet size");
_Static_assert(_Alignof(Packet) == 8, "Packet alignment");
_Static_assert(offsetof(Packet, tag) == 0, "Packet.tag offset");
_Static_assert(offsetof(Packet, serial) == 8, "Packet.serial offset");
_Static_assert(offsetof(Packet, flags) == 16, "Packet.flags offset");
_Static_assert(offsetof(Packet, point) == 20, "Packet.point offset");
_Static_assert(sizeof(Link) == 16, "Link size");
_Static_assert(_Alignof(Link) == 8, "Link alignment");
_Static_assert(offsetof(Link, next) == 0, "Link.next offset");
_Static_assert(offsetof(Link, code) == 8, "Link.code offset");

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

    gti_source = r"""
[[c_abi]]
struct Vec2 {
  mut float x;
  mut float y;
};

[[c_abi]]
struct Packet {
  mut uint8_t tag;
  mut uint64_t serial;
  mut uint16_t flags;
  mut Vec2 point;
};

[[c_abi]]
struct Link {
  mut Packet* next = nullptr;
  mut uint32_t code = 0;
};

extern "C" {
  Packet c_packet_make(uint8_t tag, uint64_t serial, uint16_t flags,
                       float x, float y);
  Packet c_packet_roundtrip(Packet value);
  uint64_t c_packet_checksum(Packet value);
  void c_packet_translate(Packet* value, float dx, float dy);
  Link c_link_null(uint32_t code);
}

int main() {
  if (sizeof(Vec2) != uint64_t(8) || alignof(Vec2) != uint64_t(4) ||
      sizeof(Packet) != uint64_t(32) || alignof(Packet) != uint64_t(8) ||
      sizeof(Link) != uint64_t(16) || alignof(Link) != uint64_t(8)) {
    return 1;
  }

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

  mut Link link = Link();
  unsafe {
    link = c_link_null(uint32_t(77));
  }
  if (link.code != uint32_t(77)) {
    return 4;
  }

  std::println("native C record oracle passed");
  return 0;
}
"""

    with tempfile.TemporaryDirectory(prefix="gti-native-record-") as temp_dir:
        temp = Path(temp_dir)
        c_path = temp / "native_record_oracle.c"
        object_path = temp / "native_record_oracle.o"
        source_path = temp / "native_record_oracle.gti"
        c_path.write_text(c_source, encoding="utf-8")
        source_path.write_text(gti_source, encoding="utf-8")

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
