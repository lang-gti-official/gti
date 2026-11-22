#!/usr/bin/env python3
"""Explicit constructor field publication survives optimization.

Text-field initializers now have an ordinary MIR Initialize use. The optimizer
must preserve their observable values at O0, O1 and O3 without depending on the
obsolete metadata-only publication remark. The owning C++ optimizer regression
also proves that removing the required explicit stage is rejected.
"""

import json
from pathlib import Path
import subprocess
import sys
import tempfile


CASES = {
    "generated_default": (
        """import std.string;
struct P { std::string_view name = "Ada"; uint32_t health = 7;  public: P() = default; };
void app(mut std::string& out, std::string_view& item) { out.append(item); }
int main() {
  P p{};
  mut std::string out = std::string("");
  app(out, p.name);
  std::print(out.view());
  std::print("|");
  std::print(p.name);
  std::print("\\n");
  return 0;
}
""",
        "Ada|Ada\n",
    ),
    "user_constructor_default_member": (
        """import std.string;
struct P { std::string_view name = "Ada"; uint32_t health = 7; P() {} };
int main() { P p{}; std::print(p.name); std::print("\\n"); return 0; }
""",
        "Ada\n",
    ),
    "explicit_initializer": (
        """import std.string;
struct P { std::string_view name; uint32_t health; P() : name("Ada"), health(7) {} };
int main() { P p{}; std::print(p.name); std::print("\\n"); return 0; }
""",
        "Ada\n",
    ),
    "nested_record": (
        """import std.string;
struct Q { std::string_view tag = "Q";  public: Q() = default; };
struct P { Q q{}; std::string_view name = "Ada";  public: P() = default; };
int main() { P p{}; std::print(p.q.tag); std::print(p.name); std::print("\\n"); return 0; }
""",
        "QAda\n",
    ),
}



def main() -> int:
    compiler = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-initializer-publication-") as directory:
        root = Path(directory)
        for name, (text, expected) in CASES.items():
            source = root / f"{name}.gti"
            source.write_text(text, encoding="utf-8")
            for level in ("O0", "O1", "O3"):
                label = f"{name}-{level}"
                executable = root / label
                report = root / f"{label}.json"
                built = subprocess.run(
                    [str(compiler), str(source), f"-{level}", "-o", str(executable),
                     "--optimization-report", str(report)],
                    capture_output=True, text=True, timeout=300, check=False)
                assert built.returncode == 0, (label, built.stdout, built.stderr)
                result = subprocess.run([str(executable)], capture_output=True,
                                        text=True, timeout=30, check=False)
                assert (result.returncode, result.stdout) == (0, expected), (
                    label, result.returncode, result.stdout, result.stderr)
                passes = json.loads(report.read_text(encoding="utf-8"))["passes"]
                dead = [entry for entry in passes
                        if entry["name"] == "dead-unused-computations"]
                removed_in_constructor = [
                    remark for entry in dead for remark in entry["remarks"]
                    if remark["kind"] == "applied"
                    and remark["body"]["kind"] == "constructor"]
                assert not removed_in_constructor, (label, removed_in_constructor)
                if level == "O0":
                    assert not dead, (label, "O0 schedules no transformation")
                else:
                    assert dead, (label, "optimized levels run dead-computation analysis")
            print(f"initializer publication: {name} passed (3 levels)", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
