from pathlib import Path
import shutil
import sys

source = Path("main.gti")
if sys.argv[1] == "edit":
    text = source.read_text()
    assert text.count("value + 1") == 1
    source.write_text(text.replace("value + 1", "value + 3"))
elif sys.argv[1] == "corrupt":
    cache = Path("build/gti/cache")
    entries = sorted((cache / "codegen-unit-v1").iterdir())
    assert len(entries) >= 2
    # Wrong-key records have a perfectly valid checksum for another unit.
    records = [path.read_bytes() for path in entries]
    for path, other in zip(entries, records[1:] + records[:1]):
        path.write_bytes(other)
    shutil.rmtree(cache / "v6")
elif sys.argv[1] == "save":
    executable, = Path("build/gti").glob("dev/*/incremental_codegen")
    shutil.copyfile(executable, "warm-executable")
    shutil.rmtree("build")
elif sys.argv[1] == "compare":
    executable, = Path("build/gti").glob("dev/*/incremental_codegen")
    assert executable.read_bytes() == Path("warm-executable").read_bytes()
    assert not Path("build/gti/cache/codegen-unit-v1").exists()
