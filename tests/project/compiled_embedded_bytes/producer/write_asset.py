from pathlib import Path
import sys
Path("src/assets").mkdir(exist_ok=True)
# Cross the syntax-only metadata limit without exceeding the binary-input limit.
Path("src/assets/data.bin").write_bytes(bytes([int(sys.argv[1])]) + bytes(1024 * 1024 + 16))
