#!/usr/bin/env python3
"""A holder returned beside a reference pack cannot keep a parameter's text view.

The other rejections and the run fixtures are tests/ui cases
(tests/ui/packs/reference_packs/). This case stays here because the current
compiler accepts it (it was GTI-S2045 at 68fd017a5); whether that is a
regression or an intended relaxation is undecided, and a tests/ui case would
have to change its verdict.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile



def run(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=120)


def main():
    compiler, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    os.environ["GTI_STDLIB_PATH"] = str(root / "stdlib")
    ticket = """
class Ticket {
public:
  Ticket() = default;
  Ticket(Ticket& other) = delete;
  Ticket(Ticket&& other) = default;
};
"""
    cases = {
        "function_text_escape": ("""
import std.string;
struct Holder {
  std::string_view value;
  Holder(std::string_view text) : value(text) {}
};
Holder observe<Args...>(std::string_view text, Args&... args) { return Holder(text); }
Holder escape() { std::string text = "owner"; int n = 1; return observe(text.view(), n); }
int main() { auto holder = escape(); return 0; }
""", "GTI-S2045"),
    }
    with tempfile.TemporaryDirectory(prefix="gti-reference-packs-") as temporary:
        scratch = Path(temporary)
        for name, (source, code) in cases.items():
            path = scratch / f"{name}.gti"
            path.write_text(ticket + source)
            result = run([str(compiler), str(path), "--emit-mir", "--execution-profile", "concurrent", "--progress", "never",
                          "-o", str(scratch / f"{name}.mir")])
            diagnostic = result.stdout + result.stderr
            assert result.returncode and code in diagnostic and "GTI-B000" not in diagnostic, (name, diagnostic)
            print(f"{name}: rejected", flush=True)


if __name__ == "__main__":
    main()
