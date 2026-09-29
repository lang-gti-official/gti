#!/usr/bin/env python3
"""Recursive concrete generic alternatives retain exact ownership and rollback."""

import pathlib
import subprocess
import sys
import tempfile


def run(command):
    return subprocess.run(command, text=True, capture_output=True, check=False)


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = (pathlib.Path(__file__).parent / "fixtures" /
              "recursive_payload_sum.gti").read_text()
    prefix = source.split("int main()", 1)[0]
    cases = {
        "success": (source, 0, "3\n2\n1\n"),
        "partial-owner": (prefix + """
class partial {
  node published;
  int32_t checked;
public:
  partial(int32_t divisor)
      : published(make_tree()), checked(int32_t(1) / divisor) {}
};
int main() { partial value = partial(0); return 1; }
""", 70, "3\n2\n1\n"),
        "partial-alternative": (prefix + """
enum class pending { some(std::vector<node> children, int32_t marker), none };
int main() {
  mut std::vector<node> children = std::vector<node>();
  children.push_back(node(tree_data::leaf(resource(7))));
  mut int32_t divisor = 0;
  pending value = pending::some(std::move(children), int32_t(1) / divisor);
  return 1;
}
""", 70, "7\n"),
        "destructor-failure": (source.replace(
            "mut int32_t drops = 0;", "mut int32_t drops = 0;\nmut int32_t zero = 0;"
        ).replace(
            "drops = drops + 1;",
            "drops = drops + 1;\n    if (this.identity == 2) { int32_t checked = int32_t(1) / zero; }"
        ), 70, "3\n2\n1\n"),
    }
    modes = [(standard, optimization, [])
             for standard in ("c++20", "c++23")
             for optimization in ("O0", "O1", "O3")]
    modes += [("c++23", "O0", ["-fno-elide-constructors"]),
              ("c++23", "O1", ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    with tempfile.TemporaryDirectory(prefix="gti-recursive-sum-") as temporary:
        root = pathlib.Path(temporary)
        for name, (text, expected_exit, expected_output) in cases.items():
            path = root / (name + ".gti")
            path.write_text(text)
            for index, (standard, optimization, flags) in enumerate(modes):
                binary = root / f"{name}-{index}"
                command = [str(compiler), str(path), "--std", standard,
                           "-" + optimization, "-o", str(binary)]
                if flags:
                    command += ["--", *flags]
                compiled = run(command)
                if compiled.returncode:
                    sys.stderr.write(f"{name} {standard} {optimization}: compile failed\n" +
                                     compiled.stdout + compiled.stderr)
                    return 1
                executed = run([str(binary)])
                if (executed.returncode != expected_exit or
                        executed.stdout != expected_output or
                        (expected_exit == 70 and "division_by_zero" not in executed.stderr) or
                        "AddressSanitizer" in executed.stderr or
                        "runtime error:" in executed.stderr):
                    sys.stderr.write(f"{name} {standard} {optimization}: "
                                     f"exit={executed.returncode} stdout={executed.stdout!r} "
                                     f"stderr={executed.stderr!r}\n")
                    return 1
    print("Recursive payload sums: 32 native outcomes passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
