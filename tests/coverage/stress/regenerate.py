#!/usr/bin/env python3
"""Regenerate the resource reproducers in this directory.

Each shape is parameterized because the thresholds matter: the operator chain
crashes somewhere between 450 and 470 terms, and the namespace blowup begins
at exactly one level past the parser's nesting limit. Keeping the generator
means the boundary can be re-measured after a fix rather than guessed.
"""
import pathlib

HERE = pathlib.Path(__file__).parent
ANCHOR = "class Anchor { public: int32_t id() { return 0; } };\n"


def operator_chain(terms: int) -> str:
    return (
        "// F18: a %d-term left-associative chain overflows the compiler stack.\n"
        "// The parser's nesting guard counts recursive-descent depth, which an\n"
        "// iteratively-parsed binary chain never increases, so the guard never fires.\n"
        % terms
        + ANCHOR
        + "int main() { std::println("
        + "+".join(["1"] * terms)
        + "); return 0; }\n"
    )


def namespace_depth(depth: int, header: str) -> str:
    return (
        header
        + ANCHOR
        + "".join("namespace n%d { " % i for i in range(depth))
        + "int32_t leaf() { return 7; }"
        + "}" * depth
        + "\nint main() { std::println(1); return 0; }\n"
    )


def main() -> None:
    (HERE / "operator-chain-500.gti").write_text(operator_chain(500))
    (HERE / "namespace-depth-63.gti").write_text(namespace_depth(
        63,
        "// F19: 63 nested namespaces consume unbounded memory with no diagnostic.\n"
        "// At depth 62 the same shape reports GTI-P0005 in 0.3s.\n"))
    (HERE / "namespace-depth-62-control.gti").write_text(namespace_depth(
        62,
        "// F19 control: one level shallower, rejected cleanly and immediately.\n"))
    (HERE / "formatter-blank-lines.gti").write_text("\n\n")
    print("regenerated 4 inputs in", HERE)


if __name__ == "__main__":
    main()
