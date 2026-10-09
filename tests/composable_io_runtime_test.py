#!/usr/bin/env python3
"""Checked mutable slices and generic buffered transports execute through MIR."""
from lambda_pack_invocation_runtime_test import main as runtime_matrix
import pathlib
import tempfile

if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/passive_error_cleanup.gti"
    lines = fixture.read_text().splitlines()
    constructor = next(i + 1 for i, line in enumerate(lines) if "9 / divisor" in line)
    cleanup = next(i + 1 for i, line in enumerate(lines) if "7 / this.divisor" in line)
    def failure(line):
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
    span = pathlib.Path(__file__).resolve().parents[1] / "stdlib/std/span.gti"
    span_lines = span.read_text().splitlines()
    def span_check(class_name, signature):
        # Constraints are part of the declaration, not the failure site's
        # identity. Select the exact class name before its generic parameters.
        begin = next(i for i, line in enumerate(span_lines)
                     if line.startswith(f"class {class_name}<"))
        method = next(i for i in range(begin, len(span_lines)) if signature in span_lines[i])
        return next(i + 1 for i in range(method + 1, len(span_lines))
                    if "gti_internal::index_bounds_check" in span_lines[i])
    def bounds(line):
        return (70, "", f"<std/span>:{line}: runtime error[GTI-R0007]: "
                "index_out_of_bounds in private_storage\n")
    mutable_at = span_check("mutable_span", "mut T& at(size_t index) mut")
    with tempfile.TemporaryDirectory(prefix="gti-io-native-") as directory:
        runtime_matrix(names=("mutable_span", "mutable_span_owned", "composable_io", fixture.stem,
                              "composable_io_native", "mutable_span_bounds"), cases={
            fixture.stem: [
                ((), (0, "1\n2\n3\n", "")),
                (("constructor",), (70, "2\n", failure(constructor))),
                (("cleanup", "fails"), (70, "1\n2\n", failure(cleanup))),
                (("forward", "copied", "error"), (0, "1\n2\n2\n4\n", "")),
                (("forward", "cleanup", "can", "fail"), (70, "1\n2\n2\n", failure(cleanup))),
            ],
            "composable_io_native": [
                ((str(pathlib.Path(directory) / "bytes.bin"),), (0, "a\0z\n", "ok")),
            ],
            "mutable_span_bounds": [
                ((), bounds(mutable_at)),
                (("offset",), bounds(span_check("mutable_span", "void validate_subspan"))),
                (("empty", "back"), bounds(mutable_at)),
                (("read", "cursor", "end"), bounds(span_check("span_iterator", "T& operator*()"))),
                (("mutable", "cursor", "past", "end"), bounds(span_check("mutable_span_iterator", "mut T& operator*() mut"))),
                (("valid", "end", "empty", "subspan", "now"), (0, "", "")),
            ],
        }, stdin_by_name={"composable_io_native": "a\0z\n"})
