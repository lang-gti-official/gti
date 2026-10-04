#!/usr/bin/env python3
"""Compile the actual non-Apple framework branch with strict warnings anywhere.

This is bounded branch regression evidence, not a substitute for a Linux build.
The excluded Apple path uses opaque driver types; the exercised branch must
not inspect them and must conservatively bypass framework cache identity.
"""

import pathlib
import re
import subprocess
import sys
import tempfile


def main():
    source = pathlib.Path(sys.argv[1]).read_text()
    function = re.search(r"std::optional<std::string>\nresolveNativeFrameworks\(.*?\n\}",
                         source, re.DOTALL)
    assert function, "native framework resolver changed; update the branch harness"
    with tempfile.TemporaryDirectory(prefix="gti-nonapple-framework-") as temporary:
        root = pathlib.Path(temporary)
        harness = root / "framework.cpp"
        harness.write_text(
            '#include <optional>\n#include <string>\n#include <vector>\n'
            '#undef __APPLE__\n'
            'struct ExecutableBuildRequest {};\nstruct NativeLinkIdentityPlan {};\n'
            + function.group(0) + '\n'
            'int main() {\n'
            '  ExecutableBuildRequest request; NativeLinkIdentityPlan plan;\n'
            '  if (resolveNativeFrameworks(request, {}, plan)) return 1;\n'
            '  const auto reason = resolveNativeFrameworks(request, {"Security"}, plan);\n'
            '  return reason == "name-resolved frameworks require an Apple linker resolution trace" ? 0 : 2;\n'
            '}\n')
        executable = root / "framework"
        subprocess.run([sys.argv[2], "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                        str(harness), "-o", str(executable)], check=True, timeout=60)
        subprocess.run([str(executable)], check=True, timeout=10)
    print("Non-Apple framework branch: strict compilation and conservative bypass passed")


if __name__ == "__main__":
    main()
