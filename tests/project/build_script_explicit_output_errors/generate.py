import os
from pathlib import Path

out = Path(os.environ["GTI_BUILD_OUTPUT"])
mode = Path("mode").read_text().strip()
if mode == "fail":
    raise SystemExit(7)
if mode == "dangling":
    out.symlink_to("absent-output.toml")
elif mode == "invalid":
    out.write_text('version = 1\nc-sources = ["missing.c"]\n')
else:
    out.write_text("")
