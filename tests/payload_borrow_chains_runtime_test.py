#!/usr/bin/env python3
"""Run nested payload returns and mixed bindings on both executable backends."""

from pathlib import Path
import subprocess
import sys
import tempfile


LOCAL_ESCAPE = """\
enum Shade { plain(int32_t level), mixed(int32_t level, int32_t tint) };
enum Paint { coat(Shade shade, int32_t layers) };
int32_t& bad(Paint& paint) {
  Paint copy = paint;
  switch (copy) {
  case Paint::coat(auto& shade, layers):
    switch (shade) {
    case Shade::plain(auto& level): return level;
    case Shade::mixed(auto& level, tint): return level;
    }
  }
}
int main() { Paint p = Paint::coat(Shade::plain(3), 2); return bad(p); }
"""


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: payload_borrow_chains_runtime_test.py <gti> <example>")
    compiler, example = map(lambda value: Path(value).resolve(), sys.argv[1:])
    failures = []
    with tempfile.TemporaryDirectory(prefix="gti-payload-borrow-chains-") as temp:
        directory = Path(temp)
        rejected = directory / "local-escape.gti"
        rejected.write_text(LOCAL_ESCAPE, encoding="utf-8")
        for backend in ("llvm", "cpp"):
            for optimization in ("-O0", "-O2"):
                label = f"{backend} {optimization}"
                executable = directory / f"payload-{backend}-{optimization[2:]}"
                built = subprocess.run(
                    [str(compiler), str(example), "--backend", backend,
                     optimization, "-o", str(executable)],
                    text=True, capture_output=True, check=False)
                if built.returncode != 0:
                    failures.append(f"{label}: build failed\n{built.stdout}{built.stderr}")
                    continue
                ran = subprocess.run([str(executable)], text=True,
                                     capture_output=True, check=False)
                if ran.returncode != 0 or ran.stdout or ran.stderr:
                    failures.append(f"{label}: exit {ran.returncode}, "
                                    f"stdout={ran.stdout!r}, stderr={ran.stderr!r}")
                invalid = subprocess.run(
                    [str(compiler), str(rejected), "--backend", backend,
                     optimization, "-o", str(directory / "invalid")],
                    text=True, capture_output=True, check=False)
                diagnostics = invalid.stdout + invalid.stderr
                if invalid.returncode == 0 or diagnostics.count("GTI-S2017") != 2:
                    failures.append(f"{label}: local escape was not rejected\n"
                                    f"{diagnostics}")
    if failures:
        sys.stderr.write("\n".join(failures) + "\n")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
