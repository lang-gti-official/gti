#!/usr/bin/env python3
"""A program split across exported modules behaves like the single file.

The corpus is every self-checking cell of `llvm_wrongcode_matrix.py` (each
prints `ok`, or the exact trace, exit status or diagnostic its module
expects, only when its values and its destruction counts and order are
right). Each cell's top-level declarations are moved, unchanged and marked
`export`, into module files, and `main` stays in the importing file:

  single   one module `lib` holds every declaration
  facade   `lib` as above, imported through `facade`, which only contains
           `export import "lib.gti";`
  chain    the declarations are split in source order: the first half in
           module `a`, the second in module `b`, which re-exports `a`

docs/language/programs-and-targets.md 6.1 makes an exported declaration
visible to its importer with its meaning unchanged, so each variant must
meet the original cell's expectation. This is the RFC 0013 equivalent-source
oracle for module boundaries: generic instantiation, class layout, cleanup
and dispatch across separately analyzed source units.

A variant that fails with a hard verdict is a defect. A variant rejected with
an ordinary diagnostic is reported as `declined` with its code; most are
reviewed by hand (for example a rule that a module may not export mutable
global state), and the cell's original single-file verdict is printed beside
it. Cells with a top-level namespace are excluded: an `export` prefix does
not apply to a namespace block.
"""
import concurrent.futures
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import matrix_support  # noqa: E402
import llvm_wrongcode_matrix as wrongcode  # noqa: E402

VARIANTS = ("single", "facade", "chain")


def chunks(text):
    """Split source into top-level chunks, aware of strings and comments."""
    result, current, depth = [], [], 0
    for line in text.splitlines(keepends=True):
        current.append(line)
        index, quote = 0, None
        while index < len(line):
            ch = line[index]
            if quote:
                if ch == "\\":
                    index += 1
                elif ch == quote:
                    quote = None
            elif ch in "\"'":
                quote = ch
            elif line.startswith("//", index):
                break
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
            index += 1
        block = "".join(current).strip()
        if depth == 0 and block and (block.endswith(";") or block.endswith("}")):
            result.append("".join(current))
            current = []
    if "".join(current).strip():
        result.append("".join(current))
    return result


def split(text):
    """(imports, declarations, main) or None when the cell is excluded."""
    imports, declarations, main = [], [], []
    for chunk in chunks(text):
        stripped = chunk.lstrip()
        if stripped.startswith("import "):
            imports.append(chunk)
        elif stripped.startswith("int main("):
            main.append(chunk)
        elif stripped.startswith("namespace "):
            return None
        elif stripped.startswith("//") and "\n" not in stripped.strip():
            continue
        else:
            declarations.append(chunk if stripped.startswith("export ")
                                else chunk.replace(stripped, "export " + stripped, 1))
    if len(main) != 1 or not declarations:
        return None
    return "".join(imports), declarations, main[0]


def write_variant(directory, variant, imports, declarations, main):
    directory.mkdir(parents=True, exist_ok=True)
    if variant == "chain":
        middle = max(1, len(declarations) // 2)
        (directory / "a.gti").write_text(
            "export module a;\n" + imports + "".join(declarations[:middle]),
            encoding="utf-8")
        (directory / "b.gti").write_text(
            "export module b;\n" + imports + 'export import "a.gti";\n'
            + "".join(declarations[middle:]), encoding="utf-8")
        entry = "b.gti"
    else:
        (directory / "lib.gti").write_text(
            "export module lib;\n" + imports + "".join(declarations),
            encoding="utf-8")
        entry = "lib.gti"
        if variant == "facade":
            (directory / "facade.gti").write_text(
                'export module facade;\nexport import "lib.gti";\n',
                encoding="utf-8")
            entry = "facade.gti"
    source = directory / "main.gti"
    source.write_text(imports + f'import "{entry}";\n' + main, encoding="utf-8")
    return source


def options(parser):
    parser.add_argument("--matrix", nargs="*", choices=sorted(wrongcode.MATRICES),
                        default=sorted(wrongcode.MATRICES))
    parser.add_argument("--variant", nargs="*", choices=VARIANTS,
                        default=list(VARIANTS))
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--limit", type=int, default=0,
                        help="at most this many cells per wrong-code module")


def main():
    matrix = matrix_support.Matrix(__doc__.split("\n\n")[0], "module-split",
                                   default_levels="O0,O2", extra=options)
    work, excluded = [], 0
    for name in matrix.args.matrix:
        cells = list(wrongcode.MATRICES[name].cells())
        if matrix.args.limit:
            cells = cells[:matrix.args.limit]
        for cell, text, *expectation in cells:
            parts = split(text)
            if parts is None:
                excluded += 1
                continue
            for variant in matrix.args.variant:
                key = f"{name}/{cell}/{variant}"
                work.append((key, write_variant(matrix.out / name / cell / variant,
                                                variant, *parts),
                             expectation[0] if expectation else {}))

    def classify(item):
        key, source, expect = item
        return key, matrix.classify(
            source, key=key, flags=("--execution-profile", "concurrent"),
            expect_stdout=expect.get("stdout", "ok\n"),
            expect_returncode=expect.get("returncode", 0),
            expect_stderr=expect.get("stderr"),
            expect_diagnostic=expect.get("diagnostic"))

    failures, declined = [], {}
    with concurrent.futures.ThreadPoolExecutor(matrix.args.jobs) as pool:
        for key, verdict in pool.map(classify, work):
            print(f"{key:60} {verdict}", flush=True)
            if matrix_support.hard(verdict):
                failures.append((key, verdict))
            elif verdict != "ok":
                declined[verdict] = declined.get(verdict, 0) + 1
    print(f"{len(work)} split programs at {','.join(matrix.levels)} "
          f"({excluded} cells excluded): {len(failures)} hard failures, "
          f"{sum(declined.values())} declined {dict(sorted(declined.items()))}")
    for key, verdict in failures:
        print(f"  {verdict}  {key}")
    return matrix.finish(failures)


if __name__ == "__main__":
    sys.exit(main())
