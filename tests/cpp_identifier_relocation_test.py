#!/usr/bin/env python3
"""Generated names are invariant under checkout and toolchain relocation."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def run(argv, cwd, env):
    result = subprocess.run(argv, cwd=cwd, env=env, capture_output=True, text=True,
                            timeout=90)
    if result.returncode:
        raise AssertionError(f"{argv}: exit {result.returncode}\n{result.stderr[-8000:]}")
    return result


def main():
    compiler = str(Path(sys.argv[1]).resolve())
    repository = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="gti-identifiers-") as temporary:
        base = Path(temporary)
        moved_stdlib = base / "toolchain" / "stdlib"
        shutil.copytree(repository / "stdlib", moved_stdlib)
        for name, source in (
            ("ownership", (repository / "examples/013-ownership.gti").read_text()),
            ("nominal", """import "../external/left/shared.gti";
import "../external/right/shared.gti";
struct Outer { struct Inner { int value; }; };
class Box<T> {
public:
  T item;
  Box(T value) : item(value) {}
};
enum class State<T> { empty, value(T item) };
static int inspect<T>(int item) { return item; }
int main() {
  Outer::Inner inner{.value = 7};
  Box<Outer::Inner> box{inner};
  State<int> state = State<int>::empty;
  return box.item.value + inspect<Outer::Inner>(inner.value)
    + left() + right() - 47;
}
"""),
        ):
            projects = [base / name / location for location in ("first", "relocated")]
            for project in projects:
                (project / "app").mkdir(parents=True)
                (project / "app/main.gti").write_text(source)
                # Same-spelled internal functions in separate external source
                # units must stay distinct, including when their basenames match.
                for side, value in (("left", 11), ("right", 22)):
                    external = project / "external" / side
                    external.mkdir(parents=True)
                    (external / "shared.gti").write_text(
                        f"export module {side}_shared;\n"
                        f"static int local<T>(T ignored) {{ return {value}; }}\n"
                        f"export int {side}() {{ return local<int>(0); }}\n")
            for optimization in ("-O0", "-O3"):
                outputs = []
                for project in projects:
                    for stdlib in (repository / "stdlib", moved_stdlib):
                        env = dict(os.environ, GTI_STDLIB_PATH=str(stdlib))
                        output = base / "program.cpp"
                        run([compiler, "main.gti", optimization, "--emit-cpp",
                             "-o", str(output)], project / "app", env)
                        outputs.append(output.read_bytes())
                assert all(output == outputs[0] for output in outputs), (
                    f"{name} {optimization}: relocating source or stdlib changed C++")
            if name == "nominal":
                env = dict(os.environ, GTI_STDLIB_PATH=str(moved_stdlib))
                executable = base / ("nominal-program.exe" if os.name == "nt"
                                     else "nominal-program")
                run([compiler, "main.gti", "-O3", "-o", str(executable)],
                    projects[1] / "app", env)
                run([str(executable)], projects[1] / "app", env)
    print("source/stdlib relocation and distinct external identities passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
