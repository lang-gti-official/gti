#!/usr/bin/env python3
"""Nested one-owner carriers preserve borrow access and cannot hide an escape."""

import os
import pathlib
import subprocess
import sys
import tempfile

from lambda_pack_invocation_runtime_test import main as run_runtime_matrix


def reject_invalid_sources(compiler: pathlib.Path) -> None:
    prefix = """#include <std/utility>
#include <std/string>
class ReadView {
  int& item;
public:
  ReadView(int& item) : item(item) {}
  int& read() { return this.item; }
};
class WriteView {
  mut int& item;
public:
  WriteView(mut int& item) : item(item) {}
  void write(int value) mut { mut int& target = this.item; target = value; }
};
"""
    cases = {
        "nested-lifecycle-guard": """#include <std/mutex>
int main() {
  std::mutex<int> owner{1};
  auto result = std::pair<std::mutex_guard<int>, bool>(owner.lock(), true);
  return 0;
}""",
        "owned-sibling-copy": """int main() {
  int value = 1;
  auto result = std::pair<ReadView, std::string>(ReadView(value), std::string("owned"));
  auto copy = result;
  return copy.first.read();
}""",
        "owner-mutation": """int main() {
  mut int value = 1;
  auto result = std::pair<ReadView, bool>(ReadView(value), true);
  value = 2;
  return result.first.read();
}""",
        "local-owner-return": """std::pair<ReadView, bool> bad() {
  int value = 1;
  return std::pair<ReadView, bool>(ReadView(value), true);
}
int main() { auto result = bad(); return result.first.read(); }
""",
        "exclusive-copy": """int main() {
  mut int value = 1;
  auto result = std::pair<WriteView, bool>(WriteView(value), true);
  auto copy = result;
  return 0;
}""",
        "exclusive-extraction-copy": """int main() {
  mut int value = 1;
  auto result = std::pair<WriteView, bool>(WriteView(value), true);
  auto copy = result.first;
  return 0;
}""",
        "reseat-field": """int main() {
  int a = 1;
  int b = 2;
  mut auto result = std::pair<ReadView, bool>(ReadView(a), true);
  result.first = ReadView(b);
  return result.first.read();
}""",
        "constructor-selected-owner-mutation": """class Selected {
  ReadView view;
public:
  Selected(ReadView unused, int& chosen) : view(ReadView(chosen)) {}
  int& read() { return this.view.read(); }
};
int main() {
  int a = 1;
  mut int b = 2;
  Selected value{ReadView(a), b};
  b = 3;
  return value.read();
}
""",
        "constructor-selected-local-owner-return": """class Selected {
  ReadView view;
public:
  Selected(ReadView unused, int& chosen) : view(ReadView(chosen)) {}
  int& read() { return this.view.read(); }
};
Selected escape(ReadView input) {
  int local = 2;
  return Selected(input, local);
}
int main() {
  int a = 1;
  auto value = escape(ReadView(a));
  return value.read();
}
""",
        "readonly-field-reborrow": """class Bad {
  mut int& item;
public:
  Bad(mut int& value) : item(value) {}
  void write() { mut int& target = this.item; target = 2; }
};
int main() { mut int value = 1; Bad bad{value}; bad.write(); return 0; }
""",
        "readonly-implicit-field-reborrow": """class Bad {
  mut int& item;
public:
  Bad(mut int& value) : item(value) {}
  void write() { mut int& target = item; target = 2; }
};
int main() { mut int value = 1; Bad bad{value}; bad.write(); return 0; }
""",
    }
    expected_codes = {
        "nested-lifecycle-guard": "GTI-S2045",
        "owned-sibling-copy": "GTI-S2003",
        "owner-mutation": "GTI-S2017",
        "local-owner-return": "GTI-S2045",
        "exclusive-copy": "GTI-S2003",
        "exclusive-extraction-copy": "GTI-S2003",
        "reseat-field": "GTI-S2003",
        "constructor-selected-owner-mutation": "GTI-S2017",
        "constructor-selected-local-owner-return": "GTI-S2085",
        "readonly-field-reborrow": "GTI-S2017",
        "readonly-implicit-field-reborrow": "GTI-S2017",
    }
    with tempfile.TemporaryDirectory(prefix="gti-nested-borrow-negative-") as temporary:
        root = pathlib.Path(temporary)
        for name, source in cases.items():
            path = root / f"{name}.gti"
            path.write_text(prefix + source)
            profile = ["--execution-profile", "concurrent"] if name == "nested-lifecycle-guard" else []
            result = subprocess.run(
                [str(compiler), str(path), *profile, "--emit-mir", "-o", str(root / f"{name}.mir")],
                capture_output=True, text=True, timeout=60, check=False,
            )
            assert result.returncode == 65 and f"error[{expected_codes[name]}]" in result.stderr, (
                name, result.returncode, result.stdout, result.stderr)
            assert "GTI-B0001" not in result.stderr and "error[GTI-P" not in result.stderr, (
                name, result.stderr)
    print(f"nested borrowed fields: {len(cases)} invalid ownership cases rejected", flush=True)


if __name__ == "__main__":
    repository = pathlib.Path(__file__).resolve().parent.parent
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    reject_invalid_sources(pathlib.Path(sys.argv[1]).resolve())
    run_runtime_matrix(names=("nested_borrow_fields", "nested_borrow_fields_mutable",
                              "nested_borrow_owned_sibling", "borrowed_arrow_compound",
                              "fallible_nested_borrow"))
