#!/usr/bin/env python3

import pathlib
import re
import subprocess
import sys
import tempfile


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(process.stdout)
    sys.stderr.write(process.stderr)
    return 1


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: issues_122_124_field_initializers_runtime_test.py "
            "<gti> <source>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-field-initializers-") as temporary:
        root = pathlib.Path(temporary)
        for optimization in ("O0", "O2"):
            for standard in ("c++20", "c++23"):
                executable = root / f"field-initializers-{optimization}-{standard}"
                built = run(
                    [
                        str(compiler),
                        str(source),
                        f"-{optimization}",
                        "--std",
                        standard,
                        "-o",
                        str(executable),
                    ]
                )
                if built.returncode != 0:
                    return fail(built)
                executed = run([str(executable)])
                if executed.returncode != 0:
                    return fail(executed)

        emitted = root / "field-initializers.cpp"
        emission = run(
            [
                str(compiler),
                str(source),
                "-O0",
                "--std",
                "c++23",
                "--emit-cpp",
                "-o",
                str(emitted),
            ]
        )
        if emission.returncode != 0:
            return fail(emission)
        generated = emitted.read_text(encoding="utf8")
        # Declaration defaults are now explicit constructor MIR, not flattened
        # native in-class expressions. Bind each source field to its exact
        # publication place and retain the staged grouped/conversion operations.
        schedules = []
        for field, converted in (("grouped", False), ("converted", True)):
            place = re.search(r"auto &(__gti_mir_p_\d+) = \(\*this\)\." + field + r";", generated)
            if place is None:
                return 1
            rhs = r"static_cast<std::int32_t>\((?P=literal)\)" if converted else r"(?P=literal)"
            schedules.append(
                r"(?P<literal>__gti_mir_v_\d+) = static_cast<std::int32_t>\(7\);\s*"
                r"(?P<value>__gti_mir_v_\d+) = " + rhs + r";\s*"
                + re.escape(place.group(1)) + r" = (?P=value);\s*"
                r"// GTI MIR full-expression boundary \d+"
            )
        schedules.extend((
            r"mir_checked_convert_v1<std::int8_t>\(__gti_mir_v_\d+, &__gti_mir_v_\d+\)",
            r"(?P<value>__gti_mir_v_\d+) = ::__gti_program::payload_t\{::__gti_program::payload_t::__gti_variant_\d+\{\}\};\s*\(\*this\)\.empty = std::move\((?P=value)\);",
            r"(?P<value>__gti_mir_v_\d+) = ::__gti_program::payload_t\{::__gti_program::payload_t::__gti_variant_\d+\{__gti_mir_v_\d+\}\};\s*\(\*this\)\.valued = std::move\((?P=value)\);",
        ))
        if any(re.search(pattern, generated) is None for pattern in schedules):
            sys.stderr.write(
                "generated C++ did not retain the verified field initializer "
                "schedule\n"
            )
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
