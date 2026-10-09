#!/usr/bin/env python3
"""General closure-object facilities, proved with application-defined owners.

* Relocation eligibility of a closure derives from its captures: scalar
  captures are bitwise relocatable, an owning string capture is not.
* `std::layout_of` of a closure-bearing class instantiates with the caller's
  closure type.
* Raw addresses of closure-bearing objects pass through the general lifetime
  helpers, while checked references to closures stay restricted.
* `std::inline_polymorphic_payload` and `std::inline_polymorphic` reject
  ineligible payloads and malformed slots before code generation.
"""

from pathlib import Path
import sys
import tempfile

from unique_class_allocation_runtime_test import run


EXPECTED = """scalar bitwise true holder true bytes 4
pair bitwise true holder true bytes 16
text bitwise false holder false string-sized true
4142
4346
"""

SHAPES = """#include <std/polymorphic>
#include <std/string>

interface shape { int area(); };

class square : public shape {
  int side;
public:
  square(int side) : side(side) {}
  int area() override { return this.side * this.side; }
};
"""

REJECTED = {
    "string payload": (SHAPES + """
class labelled : public shape {
  std::string label;
public:
  labelled(std::string label) : label(std::move(label)) {}
  int area() override { return int(this.label.size()); }
};
int need<M>() requires std::inline_polymorphic_payload<M, shape> { return 1; }
int main() { return need<labelled>(); }
""", "GTI-S2029"),
    "destructor payload": (SHAPES + """
class noisy : public shape {
public:
  noisy() {}
  ~noisy() { std::println("bye"); }
  int area() override { return 1; }
};
int need<M>() requires std::inline_polymorphic_payload<M, shape> { return 1; }
int main() { return need<noisy>(); }
""", "GTI-S2029"),
    "unrelated payload": (SHAPES + """
class lone {
public:
  lone() {}
  int area() { return 1; }
};
int need<M>() requires std::inline_polymorphic_payload<M, shape> { return 1; }
int main() { return need<lone>(); }
""", "GTI-S2029"),
    "scalar interface": (SHAPES + """
int main() {
  std::inline_polymorphic<int, 32, 8> value{};
  return 0;
}
""", "GTI-S2019"),
    "empty capacity": (SHAPES + """
int main() {
  std::inline_polymorphic<shape, 0, 8> value{};
  return 0;
}
""", "GTI-S2019"),
    "odd alignment": (SHAPES + """
int main() {
  std::inline_polymorphic<shape, 32, 3> value{};
  return 0;
}
""", "GTI-S2019"),
    "closure reference": ("""
int peek<T>(T& value) { return 1; }
int main() {
  int offset = 1;
  auto callback = [offset](int value) -> int { return value + offset; };
  return peek(callback);
}
""", "GTI-S2046"),
    "owning closure bytes": ("""
#include <std/string>
int keep<std::bitwise_relocatable Callable>(Callable callable) { return 1; }
int main() {
  std::string text = std::string("abc");
  return keep([text = std::move(text)](int value) -> int { return value; });
}
""", "GTI-S2029"),
}


def main():
    compiler = Path(sys.argv[1]).resolve()
    source = Path(__file__).resolve().parent / "fixtures/inline_closure_owner.gti"
    modes = [(f"{std}-{opt}", ["--std", std, f"-{opt}"], [])
             for std in ("c++20", "c++23") for opt in ("O0", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-inline-closure-") as temporary:
        for name, flags, native in modes:
            binary = Path(temporary) / name
            built = run([str(compiler), str(source), *flags, "-o", str(binary),
                         "--", *native])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            assert "warning[" not in built.stdout + built.stderr, (name, built.stderr)
            result = run([str(binary)])
            assert (result.returncode, result.stdout, result.stderr) == (0, EXPECTED, ""), (
                name, result.returncode, result.stdout, result.stderr)
            print(f"inline closure owner: {name} passed", flush=True)
        for label, (text, code) in REJECTED.items():
            path = Path(temporary) / (label.replace(" ", "_") + ".gti")
            path.write_text(text)
            checked = run([str(compiler), str(path), "-o",
                           str(path.with_suffix(".bin"))])
            output = checked.stdout + checked.stderr
            assert checked.returncode != 0 and f"error[{code}]" in output, (label, output)
            print(f"inline closure owner: rejects {label}", flush=True)


if __name__ == "__main__":
    main()
