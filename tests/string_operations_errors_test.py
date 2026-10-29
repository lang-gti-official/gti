#!/usr/bin/env python3
"""String range failures unwind; retained and aliased views exclude editing."""

import os
import pathlib
import subprocess
import sys
import tempfile


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    repository = pathlib.Path(__file__).resolve().parent.parent
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repository / "stdlib"))

    def run(command):
        return subprocess.run(command, env=environment, capture_output=True, text=True, timeout=120)

    with tempfile.TemporaryDirectory(prefix="gti-string-errors-") as directory:
        root = pathlib.Path(directory)
        for level in ("O0", "O3"):
            binary = root / level
            built = run([str(compiler), str(repository / "tests/fixtures/string_operations_bounds.gti"),
                         f"-{level}", "-o", str(binary)])
            assert built.returncode == 0, built.stderr
            for count in range(5):
                executed = run([str(binary), *(["bad"] * count)])
                assert executed.returncode == 70 and executed.stdout == "99\n" and \
                    "runtime error[GTI-R0007]" in executed.stderr, executed
        invalid = [
            'auto view = text.view(); text.erase(); return int(view.size());',
            'text.insert(std::size_t(0), text.view()); return 0;',
            'text.replace(std::size_t(0), std::size_t(1), text.view()); return 0;',
            'text.insert(std::size_t(0), text); return 0;',
            'text.replace(std::size_t(0), std::size_t(1), text); return 0;',
            'auto view = text.view(); auto moved = std::move(text); return int(view.size());',
        ]
        for index, body in enumerate(invalid):
            source = root / f"invalid-{index}.gti"
            source.write_text('import std.string;\nint main() { mut std::string text{"abc"}; ' + body + '}\n')
            checked = run([str(compiler), str(source), "--emit-mir", "-o", str(root / "invalid.mir")])
            assert checked.returncode != 0 and "error[GTI-S2017]" in checked.stderr, checked
            assert "GTI-B0001" not in checked.stderr, checked.stderr
    print("string range cleanup and borrow exclusions passed")


if __name__ == "__main__":
    main()
