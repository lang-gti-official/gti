import sys
from pathlib import Path

assert Path("runs").read_text() == sys.argv[1], "unexpected script execution count"
manifest, = Path("build/gti/build-scripts").rglob("gti-build-output.toml")
if sys.argv[2] == "native":
    assert 'c-sources = ["marker.c"]' in manifest.read_text()
    Path("mode").write_text("empty\n")
else:
    assert manifest.read_text() == "version = 1\n", "stale native outputs retained"
    if sys.argv[2] == "remove":
        manifest.unlink()
