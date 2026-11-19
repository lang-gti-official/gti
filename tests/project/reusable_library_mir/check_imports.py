"""Check direct call targets in the linked program on x86-64 and AArch64."""
from pathlib import Path
import re
import subprocess

(executable,) = Path("build/gti/release").glob("*/app")
assembly = subprocess.check_output(["objdump", "-d", str(executable)], text=True)
branch = r"\s(?:bl?|callq?|jmpq?)\s+[^<\n]*<(_+gtilib_callable_\w+)>"
targets = set(re.findall(branch, assembly))
assert len(targets) == 1, f"only the noinline function should need a native call: {targets}"
