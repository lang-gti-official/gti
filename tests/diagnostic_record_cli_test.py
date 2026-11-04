"""Opt-in project capture preserves failure status and historical evidence."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

compiler = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="gti-diagnostic-cli-") as tmp:
    root = Path(tmp)
    (root / "src").mkdir()
    (root / "gti.toml").write_text('manifest-version = 1\n[package]\nname = "sample"\nversion = "0.1.0"\n[targets.sample]\nkind = "executable"\nroot = "src/main.gti"\n')
    source = root / "src/main.gti"
    source.write_text('int main() { return missing_name; }\n')
    def run(*args):
        return subprocess.run([compiler, *args], cwd=root, capture_output=True, text=True, timeout=90)
    original = run("build")
    assert original.returncode != 0, original.stdout
    assert not (root / ".gti/diagnostic-reports").exists()
    captured = run("build", "--record-diagnostics")
    assert captured.returncode == original.returncode, captured.stderr
    records = list((root / ".gti/diagnostic-reports").glob("*.json"))
    assert len(records) == 1, captured.stderr
    record = json.loads(records[0].read_text())
    assert record["diagnostics"] and record["sources"], record
    assert any("missing_name" in s["text"] for s in record["sources"])
    source.write_text('int main() { return 0; }\n')
    exported = run("diagnostic-export", str(records[0]), str(root / "export"), "--note", "Expected this name to resolve")
    assert exported.returncode == 0, exported.stderr
    assert "missing_name" in (root / "export/report.json").read_text()
    assert run("diagnostic-export", str(records[0]), str(root / "export")).returncode != 0
    # Capture failure must not replace the original compilation error.
    source.write_text('int main() { return missing_name; }\n')
    (root / ".gti/diagnostic-reports/.lock").mkdir()
    blocked = run("run", "--record-diagnostics")
    assert blocked.returncode == original.returncode and "could not save diagnostic evidence" in blocked.stderr, blocked.stderr
print("PASS diagnostic recorder CLI")
