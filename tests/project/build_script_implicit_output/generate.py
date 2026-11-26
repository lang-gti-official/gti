import os
from pathlib import Path

out = Path(os.environ["GTI_BUILD_OUT_DIR"])
counter = Path("runs")
counter.write_text(str(int(counter.read_text()) + 1) if counter.exists() else "1")
if Path("mode").read_text().strip() == "native":
    (out / "marker.c").write_text("int build_script_marker(void) { return 1; }\n")
    Path(os.environ["GTI_BUILD_OUTPUT"]).write_text(
        'version = 1\nc-sources = ["marker.c"]\n')
