#!/usr/bin/env python3
"""Consuming calls hand a cleanup-free receiver moved out of a place to the callee."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def prepared_receiver_cases(name="consuming_prepared_receiver"):
    fixture = pathlib.Path(__file__).parent / f"fixtures/{name}.gti"
    lines = fixture.read_text().splitlines()

    def line(fragment):
        return next(i + 1 for i, text in enumerate(lines) if fragment in text)

    def division(fragment):
        return (f"{fixture.name}:{line(fragment)}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    guard = line("int guard = 1 / this.divisor")
    double = (f"{fixture.name}:{line('int fragile = 1 / this.divisor')}: "
              f"runtime error[GTI-R0014]: failure_during_cleanup; primary "
              f"[GTI-R0002] at {fixture.name}:{guard}; secondary [GTI-R0002]\n")
    # Each result is constructed and destroyed exactly once: returned results
    # by their caller, discarded and field-read ones at the end of their full
    # expression, and a replacement after the old value it replaces.
    common = ("1\n-1\n2\n-2\n3\n-3\n4\n-4\n5\n-5\n6\n-6\n7\n-7\n8\n-8\n"
              "10\n-10\n11\n-11\n20\n21\n-20\n-21\n22\n23\n-22\n-23\n"
              "12\n-12\n13\n-13\n14\n-14\n15\n-15\n17\n-17\n40\n")
    prior = common + "kept\n50\n-50\n22\n51\n-22\n-51\n"
    # A later argument fails after the receiver was prepared: (argument, failing
    # expression, output on success, end-of-main release, output on failure).
    # The member never runs, and an owning argument prepared before the failing
    # one is destroyed once.
    later = [
        ("bound", "Tracked made = std::move(maker).add(1 / divisor)",
         "61\n-61\n", "", ""),
        ("returned", "return std::move(maker).add(1 / divisor)", "63\n", "-63\n", ""),
        ("absorbed", "absorb(Tracked(70), 1 / divisor)",
         "70\n135\n-70\n-135\n", "", "70\n-70\n"),
        ("viewed", "show_add(1 / divisor)", "67\n-67\n", "", ""),
        ("expected", "attempt(1 / divisor == 0)", "68\n-68\n", "", ""),
        ("assembled", ": item(std::move(maker).add(1 / divisor))", "73\n", "-73\n", ""),
        ("forwarded", "item(build(std::move(maker), 1 / divisor))", "75\n", "-75\n", ""),
        ("looped", "total += std::move(maker).add(1 / divisor)",
         "81\n-81\n82\n-82\n", "", ""),
        ("generic", "std::move(tagged).add(1 / divisor)", "85\n", "-85\n", ""),
        ("discarded", "[[discard]] std::move(maker).add(1 / divisor)",
         "87\n-87\n", "", ""),
        # A failing replacement keeps the old value for failure cleanup.
        ("replacing", "kept = std::move(maker).add(1 / divisor)",
         "90\n89\n-90\n-89\n", "", "90\n-90\n"),
        ("called", "std::move(factory)(1 / divisor)", "92\n", "-92\n", ""),
        ("lambda", "std::move(source).add(1 / divisor)", "94\n", "-94\n", ""),
    ]

    def after(count, failed=""):
        done = later[:count]
        return ("".join(case[2] for case in done) + failed +
                "".join(case[3] for case in reversed(done)) + "-40\n")

    earlier = ("guard", "result", "callee", "replace")
    return {fixture.stem: [
        ((), (0, prior + after(len(later)), "")),
        # The guard fails after take_guarded's result exists, before Return.
        (("guard",), (70, common + "-40\n",
                      division("int guard = 1 / this.divisor"))),
        (("guard", "result"), (70, common + "-40\n", double)),
        # A consuming callee fails before its result exists.
        (("guard", "result", "callee"),
         (70, common + "kept\n-40\n", division("Shown(this.base / divisor)"))),
        (("guard", "result", "callee", "replace"),
         (70, common + "kept\n50\n-50\n22\n-22\n-40\n",
          division("Tracked(this.base / divisor)"))),
        *[((*earlier, *(case[0] for case in later[:index + 1])),
           (70, prior + after(index, case[4]), division(case[1])))
          for index, case in enumerate(later)],
    ]}


if __name__ == "__main__":
    main(names=("consuming_prepared_receiver",), cases=prepared_receiver_cases())
