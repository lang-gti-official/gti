#!/usr/bin/env python3
"""Unique-owner access follows MIR cleanup and original failure propagation."""

import pathlib
import subprocess
import sys
import tempfile


def run(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=120)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: unique_owner_access_runtime_test.py <gti>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    memory = fixtures.parent.parent / "stdlib/std/detail/memory_core.gti"
    lines = memory.read_text().splitlines()
    read_sites = [i for i, line in enumerate(lines, 1)
                  if "return gti_internal::unique_owner_borrow(" in line]
    mut_sites = [i for i, line in enumerate(lines, 1)
                 if "return gti_internal::unique_owner_borrow_mut(" in line]
    assert len(read_sites) == len(mut_sites) == 2
    modes = [(f"{standard}-{level}", [f"-{level}", "--std", standard])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes.append(("sanitized", ["-O1", "--std", "c++23"]))
    expected = [
        ("2\n3\n1\n", None, None),
        ("2\n1\n", read_sites[0], "dereference"),
        ("1\n", mut_sites[0], "dereference"),
        ("2\n1\n", read_sites[1], "member_access"),
        # A field read through a mutable owner selects the read-only operator->.
        ("1\n", read_sites[1], "member_access"),
        ("3\n1\n", mut_sites[1], "member_access"),
        # A read-only reference to `*owner` selects the read-only operator*.
        ("3\n1\n", read_sites[0], "dereference"),
    ]
    with tempfile.TemporaryDirectory(prefix="gti-owner-access-") as temporary:
        for mode, flags in modes:
            for fixture in ("unique_owner_access", "unique_ptr_empty_access_cleanup",
                            "unique_owner_cleanup_failure"):
                binary = pathlib.Path(temporary) / f"{fixture}-{mode}"
                command = [str(compiler), str(fixtures / f"{fixture}.gti"),
                           *flags, "--keep-cpp", "-o", str(binary)]
                if mode == "sanitized":
                    command += ["--", "-fsanitize=address,undefined",
                                "-fno-omit-frame-pointer"]
                built = run(command)
                assert built.returncode == 0, (command, built.stdout, built.stderr)
                generated = binary.with_suffix(".gti.cpp").read_text()
                assert "// GTI verified-MIR body:" in generated
                assert "::gti_internal::backend::owner_access(" not in generated
                assert "GTI_FAILURE_CODE_EMPTY_OWNER_ACCESS_V1" in generated
                cases = expected if fixture == "unique_owner_access" else [
                    ("cleanup\n", read_sites[0], "dereference")]
                for arguments, (stdout, site, detail) in enumerate(cases):
                    result = run([str(binary), *(["fail"] * arguments)])
                    stderr = "" if site is None else (
                        f"<std/detail/memory_core>:{site}: runtime error[GTI-R0008]: "
                        f"empty_owner_access in {detail}\n")
                    status = 0 if site is None else 70
                    if fixture == "unique_owner_cleanup_failure":
                        primary = next(i for i, line in enumerate(
                            (fixtures / f"{fixture}.gti").read_text().splitlines(), 1)
                            if "return 1 / divisor;" in line)
                        stderr = (
                            f"<std/detail/memory_core>:{site}: runtime error[GTI-R0014]: "
                            "failure_during_cleanup; primary [GTI-R0002] at "
                            f"{fixture}.gti:{primary}; secondary [GTI-R0008]\n")
                        assert (result.returncode, result.stdout, result.stderr) == (
                            70, stdout, stderr), (fixture, mode, result)
                    else:
                        assert (result.returncode, result.stdout, result.stderr) == (
                            status, stdout, stderr), (fixture, mode, arguments, result)
                print(f"{fixture}: {mode} passed", flush=True)


if __name__ == "__main__":
    main()
