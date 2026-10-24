#!/usr/bin/env python3
"""Native borrowed payload and owning receiver-field exchange coverage.

borrowed_payload_element observes payload enums held by fixed-array elements:
each switch checks its index, then reads the element itself. Run with an
argument, it indexes past the end of an array, which fails at that switch
before any arm runs and releases the live guard once.

borrowed_payload_reference_copies copies payload enums out of reference
accessors that bind the active payload with `auto&`, by value and through
`auto&` locals, for a cleanup-free int payload and an owned std::string one.
"""
import pathlib
import subprocess
import sys
import tempfile

EXPECTED = {
    "borrowed_payload_pattern": "2\n4\n7\n",
    "borrowed_payload_element": "4\n56\n0\n4\n7\n5\n8\n10\n3\n",
    "borrowed_payload_reference_copies":
        "15\n3\n6\n6\n6\n6\n-1\n14\n-1\n7\n16\n6\n7\n3\n4\n2\n8\n",
}


def past_end_report(source):
    line = next(number for number, text in enumerate(source.read_text().splitlines(), 1)
                if "switch (row[index]) {" in text)
    return (f"{source.name}:{line}: runtime error[GTI-R0007]: "
            "index_out_of_bounds in fixed_array\n")


def main():
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    modes = [(standard, level, []) for standard in ("c++20", "c++23")
             for level in ("O0", "O1", "O3")]
    modes += [("c++23", "O0", ["-fno-elide-constructors"]),
              ("c++23", "O1", ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    outcomes = 0
    with tempfile.TemporaryDirectory(prefix="gti-payload-borrow-") as temporary:
        for fixture in ("borrowed_payload_pattern", "borrowed_payload_receiver",
                        "borrowed_payload_projection", "borrowed_payload_mixed",
                        "owning_payload_exchange", "borrowed_payload_element",
                        "borrowed_payload_reference_copies"):
            for index, (standard, level, flags) in enumerate(modes):
                binary = str(pathlib.Path(temporary) / f"{fixture}-{index}")
                command = [compiler, str(fixtures / (fixture + ".gti")),
                           "--std", standard, "-" + level, "-o", binary]
                if flags:
                    command += ["--", *flags]
                compiled = subprocess.run(command, text=True, capture_output=True)
                assert compiled.returncode == 0, (command, compiled.stdout, compiled.stderr)
                executed = subprocess.run([binary], text=True, capture_output=True)
                expected = EXPECTED.get(fixture, "")
                assert executed.returncode == 0 and executed.stdout == expected and not executed.stderr, (
                    command, executed.returncode, executed.stdout, executed.stderr)
                outcomes += 1
                if fixture == "borrowed_payload_element":
                    failed = subprocess.run([binary, "past-end"], text=True,
                                            capture_output=True)
                    wanted = (70, "70\n-70\n",
                              past_end_report(fixtures / (fixture + ".gti")))
                    actual = (failed.returncode, failed.stdout, failed.stderr)
                    assert actual == wanted, (command, actual, wanted)
                    outcomes += 1
    print(f"Borrowed payload patterns: {outcomes} native outcomes passed")


if __name__ == "__main__":
    main()
