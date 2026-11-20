from pathlib import Path
from sys import argv

Path("assets/data.bin").write_bytes(bytes([0, int(argv[1]), 255]))
