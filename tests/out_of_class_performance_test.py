#!/usr/bin/env python3
"""An optional body split must not introduce a call/dispatch/allocation tax."""

from interface_static_performance_test import GTI, main


if __name__ == "__main__":
    inline = GTI.replace("TYPE", "class")
    split = inline.replace(
        "static uint64_t step(uint64_t value) {\n    return (value * 17 + 11) % 1000003;\n  }",
        "static uint64_t step(uint64_t value);")
    split = split.replace("int main()", "uint64_t Ops::step(uint64_t value) {\n"
                          "  return (value * 17 + 11) % 1000003;\n}\nint main()")
    main({"inline": inline, "split": split})
