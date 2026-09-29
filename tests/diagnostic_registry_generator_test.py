#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys
import tempfile


def run(generator, source, output):
    return subprocess.run(
        [sys.executable, generator, source, output],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )


def main():
    generator = sys.argv[1]
    checked_registry = sys.argv[2]
    with tempfile.TemporaryDirectory(prefix="gti-diagnostic-registry-") as directory:
        root = pathlib.Path(directory)
        generated = root / "generated.h"
        result = run(generator, checked_registry, generated)
        assert result.returncode == 0, result.stderr
        contents = generated.read_text(encoding="utf-8")
        assert "enum class DiagnosticId" in contents
        assert "DiagnosticId::UnnecessaryMutableBinding" in contents

        entries = json.loads(pathlib.Path(checked_registry).read_text(encoding="utf-8"))
        duplicate = root / "duplicate.json"
        duplicate.write_text(json.dumps([entries[0], entries[0]]), encoding="utf-8")
        result = run(generator, duplicate, generated)
        assert result.returncode != 0
        assert "duplicate diagnostic id or code" in result.stderr

        invalid_format = dict(entries[0])
        invalid_format["arguments"] = 1
        invalid = root / "invalid-format.json"
        invalid.write_text(json.dumps([invalid_format]), encoding="utf-8")
        result = run(generator, invalid, generated)
        assert result.returncode != 0
        assert "format placeholders" in result.stderr

    print("GTI diagnostic registry generator test passed")


if __name__ == "__main__":
    main()
