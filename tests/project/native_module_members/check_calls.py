"""Each of the two overloads and two free functions must call its native entry."""
import re
import subprocess
import sys

assembly = subprocess.check_output(["objdump", "-d", sys.argv[1]], text=True)
calls = set(re.findall(r"\s(?:bl?|callq?|jmpq?)\s+[^<\n]*<(_+gtilib_module_v1_[0-9a-f]{64})>", assembly))
assert len(calls) == 4, f"expected four distinct native callees, found {len(calls)}"
