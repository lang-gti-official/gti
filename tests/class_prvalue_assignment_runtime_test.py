#!/usr/bin/env python3

import pathlib
import re
import subprocess
import sys
import tempfile


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: class_prvalue_assignment_runtime_test.py "
            "<gti> <fixture-directory>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(sys.argv[2]).resolve()
    positive = fixtures / "class_prvalue_assignment.gti"
    negative = fixtures / "class_prvalue_assignment_immutable_negative.gti"

    with tempfile.TemporaryDirectory(prefix="gti-class-prvalue-") as temporary:
        root = pathlib.Path(temporary)
        for optimization in ("O0", "O3"):
            for standard in ("c++20", "c++23"):
                mode = f"{optimization}-{standard}"
                executable = root / mode
                built = run(
                    [
                        str(compiler),
                        str(positive),
                        f"-{optimization}",
                        "--std",
                        standard,
                        "-o",
                        str(executable),
                    ]
                )
                if built.returncode != 0:
                    return fail(f"build {mode}", built)
                executed = run([str(executable)])
                if (
                    executed.returncode != 0
                    or executed.stdout != ""
                    or executed.stderr != ""
                ):
                    return fail(f"run {mode}", executed)

        generated_path = root / "class-prvalue-assignment.cpp"
        emitted = run(
            [
                str(compiler),
                str(positive),
                "-O0",
                "--std",
                "c++23",
                "--emit-cpp",
                "-o",
                str(generated_path),
            ]
        )
        if emitted.returncode != 0:
            return fail("emit C++23/O0", emitted)
        generated = generated_path.read_text(encoding="utf8")

        # Slot names are body-local, so scope every count to one emitted body.
        bodies: list[list[str]] = []
        for line in generated.split("\n"):
            if "// GTI verified-MIR body:" in line:
                bodies.append([])
            if bodies:
                bodies[-1].append(line)

        # Every cleanup-owning prvalue assignment keeps MIR's exact schedule:
        # the temporary is constructed into its own staging slot, the
        # destination binding is move-assigned from that slot -- which drops
        # the value it replaces -- and only the moved-from shell is destroyed,
        # immediately and exactly once.
        publication = re.compile(
            r"^\s*__gti_mir_p_\d+\.get\(\) = "
            r"std::move\((__gti_mir_p_\d+)\.get\(\)\);$"
        )
        raising = 0
        plain = 0
        for lines in bodies:
            body = "\n".join(lines)
            for index, line in enumerate(lines):
                matched = publication.match(line)
                if matched is None:
                    continue
                slot = matched.group(1)
                retirement = re.compile(
                    r"^\s*(?:__gti_mir_drop_success_\d+ = )?"
                    + re.escape(slot)
                    + r"\.destroy(?:_with_failure)?\("
                )
                following = lines[index + 1] if index + 1 < len(lines) else ""
                if retirement.match(following) is None:
                    sys.stderr.write(
                        f"staging slot {slot} is not retired immediately after "
                        f"its publication: {following!r}\n"
                    )
                    return 1
                # Exactly one destructor call reaches the staged shell. A
                # failed construction's `std::destroy_at` cleanup is a
                # different call and never runs on the same path.
                destructions = re.findall(
                    r"(?<![\w.])"
                    + re.escape(slot)
                    + r"\.destroy(?:_with_failure)?\(",
                    body,
                )
                if len(destructions) != 1:
                    sys.stderr.write(
                        f"staging slot {slot} is destroyed "
                        f"{len(destructions)} times; expected exactly once\n"
                    )
                    return 1
                constructions = re.findall(
                    r"(?<![\w.])" + re.escape(slot) + r"\.construct\(", body
                )
                placements = re.findall(
                    r"std::construct_at\("
                    + re.escape(slot)
                    + r"\.construction_address\(\),",
                    body,
                )
                if len(constructions) + len(placements) != 1:
                    sys.stderr.write(
                        f"staging slot {slot} is constructed "
                        f"{len(constructions) + len(placements)} times; "
                        "expected exactly once\n"
                    )
                    return 1
                if placements:
                    raising += 1
                else:
                    plain += 1

        # Two raising constructions (the plain and conditional std::string
        # assignments) and two non-raising ones (the plain and loop tracked
        # assignments).
        if raising != 2 or plain != 2:
            sys.stderr.write(
                "generated C++ lost a staged prvalue assignment: "
                f"{raising} raising and {plain} plain, expected 2 and 2\n"
            )
            return 1

        # A cleanup-free passive class has no staging slot at all: MIR gives
        # the prvalue no storage, so the construction spells directly in the
        # assignment's value position.
        fused = re.findall(
            r"__gti_mir_p_\d+\.get\(\) = ::__gti_program::pair\("
            r"__gti_mir_v_\d+, __gti_mir_v_\d+\);",
            generated,
        )
        if len(fused) != 1:
            sys.stderr.write(
                "generated C++ lost the fused passive prvalue assignment "
                f"(found {len(fused)}, expected 1)\n"
            )
            return 1

        rejected = run(
            [
                str(compiler),
                str(negative),
                "-O0",
                "--std",
                "c++23",
                "-o",
                str(root / "immutable-prvalue-assign"),
            ]
        )
        if (
            rejected.returncode != 65
            or "error[GTI-S2002]" not in rejected.stderr
            or "Cannot assign to immutable binding" not in rejected.stderr
            or "Internal compiler error" in rejected.stderr
        ):
            return fail("reject immutable prvalue assignment", rejected)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
