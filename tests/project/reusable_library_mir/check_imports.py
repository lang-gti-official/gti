"""Check direct call targets in the linked program on x86-64 and AArch64."""
from pathlib import Path
import re
import struct
import subprocess
import sys

bundle_path, executable = map(Path, sys.argv[1:])
assembly = subprocess.check_output(["objdump", "-d", str(executable)], text=True)
branch = r"\s(?:bl?|callq?|jmpq?)\s+[^<\n]*<(_+gtilib_callable_\w+)>"
targets = set(re.findall(branch, assembly))
# The checked bundle format frames the optional MIR just before the digest.
bundle = bundle_path.read_bytes()
offset = bundle.rindex(b"GTIMIR01")
assert struct.unpack_from("<I", bundle, offset - 4)[0] == len(bundle) - 64 - offset
exported = set(m.decode().lstrip("_") for m in re.findall(rb"__gtilib_callable_v4_[0-9a-f]{64}", bundle[offset:-64]))
assert exported
called = {name.lstrip("_") for name in targets}
assert called.isdisjoint(exported), f"direct native calls to reusable MIR: {called & exported}"
# Each fixture deliberately includes one distinct native call (opaque or seed).
assert len(called) == 1, f"expected one native library call, got {called}"
