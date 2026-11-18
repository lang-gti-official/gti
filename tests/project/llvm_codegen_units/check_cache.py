import hashlib
from pathlib import Path
import sys

root = Path("build/gti/cache/v6")
entries = sorted(p for p in root.iterdir() if (p / "metadata").is_file())
assert len(entries) == 1, entries
entry = entries[0]
objects = [entry / "artifact.o"] + [
    entry / f"artifact-{i}.o" for i in range(1, 8)
]
assert all(p.is_file() and p.stat().st_size > 0 for p in objects)
digests = [hashlib.sha256(p.read_bytes()).hexdigest() for p in objects]
if sys.argv[1] == "record":
    Path("objects-digests").write_text("\n".join(digests))
elif sys.argv[1] == "compare":
    assert Path("objects-digests").read_text().splitlines() == digests
else:
    objects[-1].write_bytes(b"corrupt")
