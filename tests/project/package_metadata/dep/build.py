import os
import re
from pathlib import Path

manifest = Path("gti.toml").read_text()
for field in ("name", "version"):
    expected = re.search(r'^' + field + r' = "([^"\n]+)"', manifest, re.M).group(1)
    assert os.environ["GTI_BUILD_PACKAGE_" + field.upper()] == expected
Path(os.environ["GTI_BUILD_OUTPUT"]).write_text("version = 1\n")
