import sys
from pathlib import Path

Path("mode").write_text(sys.argv[1] + "\n")
