"""Each of the two overloads and two free functions must call its native entry."""
from pathlib import Path
import re
import subprocess
import sys

if sys.argv[1] == "--scalar":
    # A default-constructed plain record retains the existing native value ABI.
    bundle = Path(sys.argv[2]).read_bytes()
    assert b"__gtilib_callable_v4_" in bundle and b"gti.module-native.1" not in bundle
    sys.exit(0)

assembly = subprocess.check_output(["objdump", "-d", sys.argv[1]], text=True)
calls = set(re.findall(r"\s(?:bl?|callq?|jmpq?)\s+[^<\n]*<(_+gtilib_module_v1_[0-9a-f]{64})>", assembly))
assert len(calls) == 4, f"expected four distinct native callees, found {len(calls)}"
