"""Exercise native bundle publication, resource refresh and failure recovery."""
import plistlib
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    driver = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        assets = root / "assets"
        assets.mkdir()
        resource = assets / "example.txt"
        resource.write_text("first")
        (root / "main.gti").write_text("int main() { return 0; }\n")
        (root / "gti.toml").write_text("""manifest-version = 1
[package]
name = "example"
version = "1.2.3"
[targets.example]
kind = "executable"
root = "main.gti"
[targets.example.macos-bundle]
name = "Example & Friends"
identifier = "dev.gti.example"
resources = ["assets/example.txt"]
""")
        command = [driver, "build", "example", "--bundle", "macos"]
        if sys.platform != "darwin":
            result = subprocess.run(command, cwd=root, capture_output=True, text=True)
            assert result.returncode and "requires a macOS host" in result.stderr, result
            return
        subprocess.run(command, cwd=root, check=True)
        app, = root.glob("build/gti/**/Example & Friends.app")
        info = plistlib.loads((app / "Contents/Info.plist").read_bytes())
        assert info["CFBundleName"] == "Example & Friends"
        assert info["CFBundleIdentifier"] == "dev.gti.example"
        assert info["CFBundleShortVersionString"] == "1.2.3"
        executable = app / "Contents/MacOS" / info["CFBundleExecutable"]
        subprocess.run([executable], cwd=root, check=True)
        copy = app / "Contents/Resources/assets/example.txt"
        assert copy.read_text() == "first"
        resource.write_text("second")
        subprocess.run(command, cwd=root, check=True)
        assert copy.read_text() == "second"
        subprocess.run(["codesign", "--verify", "--strict", app], check=True)
        resource.unlink()
        resource.symlink_to(sys.executable)
        failed = subprocess.run(command, cwd=root, capture_output=True, text=True)
        assert failed.returncode and "inside the package" in failed.stderr, failed
        assert copy.read_text() == "second"
        resource.unlink()
        resource.write_text("third")
        app.rename(root / "previous.app")
        app.mkdir()
        failed = subprocess.run(command, cwd=root, capture_output=True, text=True)
        assert failed.returncode and "unmanaged bundle" in failed.stderr, failed
        assert list(app.iterdir()) == []


if __name__ == "__main__":
    main()
