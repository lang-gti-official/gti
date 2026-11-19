"""Check direct call targets in the linked program on x86-64 and AArch64."""
from pathlib import Path
import re
import subprocess

(executable,) = Path("build/gti/release").glob("*/app")
assembly = subprocess.check_output(["objdump", "-d", str(executable)], text=True)
branch = r"\s(?:bl?|callq?|jmpq?)\s+[^<\n]*<(_+gtilib_callable_\w+)>"
targets = set(re.findall(branch, assembly))
# The optimization section uses exact native ABI symbols. Only opaque() is
# absent from that section among the functions called by this consumer.
bundle = Path("kernels.gtilib").read_bytes()
bodies = bundle[bundle.index(b"GTIMIR01"):-64]
exported = set(m.decode() for m in re.findall(rb"__gtilib_callable_v4_[0-9a-f]{64}", bodies))
assert exported
assert not any(t.lstrip("_") == e.lstrip("_") for t in targets for e in exported)
assert len(targets) == 1, f"only the noinline function should need a native call: {targets}"
