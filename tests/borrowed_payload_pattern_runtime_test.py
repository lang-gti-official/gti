#!/usr/bin/env python3
"""Native borrowed payload and owning receiver-field exchange coverage."""
import pathlib
import subprocess
import sys
import tempfile


def main():
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    modes = [(standard, level, []) for standard in ("c++20", "c++23")
             for level in ("O0", "O3")]
    modes += [("c++23", "O0", ["-fno-elide-constructors"]),
              ("c++23", "O1", ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    with tempfile.TemporaryDirectory(prefix="gti-payload-borrow-") as temporary:
        for fixture in ("borrowed_payload_pattern", "borrowed_payload_receiver",
                        "borrowed_payload_projection", "borrowed_payload_mixed",
                        "owning_payload_exchange"):
            for index, (standard, level, flags) in enumerate(modes):
                binary = str(pathlib.Path(temporary) / f"{fixture}-{index}")
                command = [compiler, str(fixtures / (fixture + ".gti")),
                           "--std", standard, "-" + level, "-o", binary]
                if flags:
                    command += ["--", *flags]
                compiled = subprocess.run(command, text=True, capture_output=True)
                assert compiled.returncode == 0, (command, compiled.stdout, compiled.stderr)
                executed = subprocess.run([binary], text=True, capture_output=True)
                expected = "2\n4\n7\n" if fixture == "borrowed_payload_pattern" else ""
                assert executed.returncode == 0 and executed.stdout == expected and not executed.stderr, (
                    command, executed.returncode, executed.stdout, executed.stderr)
    print("Borrowed payload patterns: 30 native outcomes passed")


if __name__ == "__main__":
    main()
