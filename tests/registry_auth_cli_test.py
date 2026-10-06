#!/usr/bin/env python3
"""Deterministic scoped-auth transport; OS Keychain roundtrip is opt-in."""
import hashlib
import http.server
import json
import os
import pathlib
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import urllib.parse

GTI = pathlib.Path(sys.argv[1]).resolve()
MODEL = pathlib.Path(sys.argv[2]).resolve()
SECRET = "fixture-secret"


def archive():
    entries = [("gti.toml", b'manifest-version = 1\n[package]\nname = "secure"\nversion = "1.0.0"\n'),
               ("src/unit.gti", b'namespace secure { int answer() { return 42; } }\n')]
    data = b"GTIPKG1\n" + struct.pack("<I", len(entries))
    for name, contents in entries:
        path = name.encode()
        data += struct.pack("<IQB", len(path), len(contents), 0) + path + contents
    return data


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, data=b""):
        self.send_response(status)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self.server.requests += 1
        if self.headers.get("Authorization") != "token " + SECRET:
            self.reply(401, b"fixture-secret: server response must not be logged")
            return
        url = urllib.parse.urlsplit(self.path)
        parts = url.path.strip("/").split("/")
        if parts == ["api", "v1", "packages", "test"]:
            self.reply(200, b"[]")
        elif parts[:5] == ["api", "v1", "packages", "test", "generic"]:
            package = parts[5]
            if len(parts) == 6:
                self.reply(200, b'[{"version":"1.0.0"}]')
            else:
                self.reply(404)
        elif parts[:4] == ["api", "packages", "test", "generic"]:
            self.reply(200, self.server.files.get((parts[4], parts[5], parts[6]), b""))
        else:
            self.reply(404)

    def do_PUT(self):
        self.server.requests += 1
        if self.headers.get("Authorization") != "token " + SECRET:
            self.reply(403)
            return
        parts = urllib.parse.urlsplit(self.path).path.strip("/").split("/")
        key = tuple(parts[4:7])
        if key in self.server.files:
            self.reply(409)
            return
        self.server.files[key] = self.rfile.read(int(self.headers["Content-Length"]))
        self.reply(201)


def run(args, env, cwd, text=None, success=True, fragment=None):
    result = subprocess.run([str(GTI), *args], env=env, cwd=cwd, input=text,
                            text=True, capture_output=True, timeout=30)
    output = result.stdout + result.stderr
    assert (result.returncode == 0) == success, (args, result.returncode, output)
    assert SECRET not in output, output
    if fragment:
        assert fragment in output, output
    return output


def terminal_checks(env, root, cancellation):
    if os.name == "nt":
        return
    import pty
    import select
    import termios
    master, slave = pty.openpty()
    original = termios.tcgetattr(slave)
    process = subprocess.Popen([str(GTI), "registry", "login"], cwd=root,
                               env=env, stdin=slave, stdout=slave, stderr=slave)
    try:
        output = b""
        while b"input hidden" not in output:
            assert select.select([master], [], [], 10)[0], output
            output += os.read(master, 8192)
        assert not termios.tcgetattr(slave)[3] & termios.ECHO
        os.write(master, b"fixture-secret")
        process.send_signal(cancellation)
        assert process.wait(timeout=10) != 0
        assert termios.tcgetattr(slave)[3] & termios.ECHO == original[3] & termios.ECHO
        while select.select([master], [], [], 0)[0]:
            output += os.read(master, 8192)
        assert SECRET.encode() not in output, output
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)
        os.close(slave)


def main():
    with tempfile.TemporaryDirectory(prefix="gti-registry-auth-") as directory:
        root = pathlib.Path(directory)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.requests = 0
        data = archive()
        metadata = ('registry-version = 1\n[package]\nname = "secure"\nversion = "1.0.0"\n'
                    'registry-name = "secure"\n[archive]\nfile = "source.gtipkg"\n'
                    f'checksum = "sha256:{hashlib.sha256(data).hexdigest()}"\n').encode()
        server.files = {("sample", "1.0.0", "package.toml"): b"metadata",
                        ("sample", "1.0.0", "source.gtipkg"): b"archive",
                        ("secure", "1.0.0", "package.toml"): metadata,
                        ("secure", "1.0.0", "source.gtipkg"): data}
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        config = root / "registries.toml"
        config.write_text('[registries.official]\nbackend = "gitea-generic"\nowner = "test"\n'
                          f'url = "http://127.0.0.1:{server.server_port}/"\n'
                          'token-env = "GTI_TEST_REGISTRY_AUTH_TOKEN"\n')
        env = dict(os.environ, GTI_REGISTRY_CONFIG=str(config),
                   GTI_PACKAGE_CACHE=str(root / "cache"),
                   GTI_TEST_REGISTRY_AUTH_TOKEN="", NO_COLOR="1")
        try:
            result = subprocess.run([str(MODEL), "--adapter"], env=env, cwd=root,
                                    capture_output=True, text=True, timeout=30)
            assert result.returncode == 0, result.stdout + result.stderr
            assert SECRET not in result.stdout + result.stderr
            before = server.requests
            run(["registry", "status"], env, root, fragment="empty; anonymous")
            run(["registry", "status"], dict(env, GTI_TEST_REGISTRY_AUTH_TOKEN=SECRET),
                root, fragment="environment")
            assert server.requests == before, "status must not contact registry"
            for args in [["registry"], ["registry", "bad"],
                         ["registry", "logout", "--token-stdin"],
                         ["registry", "login", "--token", SECRET],
                         ["registry", "login", "official", SECRET]]:
                run(args, env, root, success=False)
            run(["registry", "--help"], env, root, fragment="registry login")
            if sys.platform == "darwin":
                for token in ["", "two tokens", SECRET + "\nHeader: injected", "a" * 4099]:
                    run(["registry", "login", "--token-stdin"], env, root,
                        text=token, success=False, fragment="GTI-B1725")
                run(["registry", "login", "--token-stdin"], env, root,
                    text="wrong\n", success=False, fragment="GTI-B1713")
                run(["registry", "login"], env, root, text=SECRET,
                    success=False, fragment="GTI-B1725")
                for cancellation in (signal.SIGINT, signal.SIGTERM, signal.SIGTSTP):
                    terminal_checks(env, root, cancellation)
            else:
                run(["registry", "login", "--token-stdin"], env, root,
                    text=SECRET, success=False, fragment="GTI-B1724")

            if sys.platform == "darwin" and os.environ.get("GTI_TEST_REGISTRY_KEYCHAIN") == "1":
                # This random loopback endpoint scopes one synthetic credential;
                # never inspect or alter a user's real registry account.
                stored_env = dict(env)
                stored_env.pop("GTI_TEST_REGISTRY_AUTH_TOKEN")
                try:
                    run(["registry", "login", "--token-stdin"], env, root,
                        text=SECRET + "\r\n", fragment="macOS Keychain")
                    run(["registry", "status"], stored_env, root, fragment="macOS Keychain")
                    other_config = root / "other-registries.toml"
                    for changed in [config.read_text().replace('owner = "test"', 'owner = "other"'),
                                    config.read_text().replace('[registries.official]', '[registries.isolated]'),
                                    config.read_text().replace(f'{server.server_port}/', f'{server.server_port}/elsewhere/')]:
                        other_config.write_text(changed)
                        name = "isolated" if "registries.isolated" in changed else "official"
                        run(["registry", "status", name],
                            dict(stored_env, GTI_REGISTRY_CONFIG=str(other_config)), root,
                            fragment="anonymous")
                    run(["registry", "login", "--token-stdin"], env, root,
                        text="wrong", success=False, fragment="GTI-B1713")
                    consumer = root / "consumer"
                    consumer.mkdir()
                    (consumer / "gti.toml").write_text('manifest-version = 1\n[package]\nname = "consumer"\nversion = "0.1.0"\n')
                    run(["add", "secure"], stored_env, consumer)
                    for path in [consumer / "gti.toml", consumer / "gti.lock"]:
                        assert SECRET not in path.read_text()
                    run(["fetch", "--locked"], stored_env, consumer)
                    for path in (root / "cache").rglob("*"):
                        if path.is_file():
                            assert SECRET.encode() not in path.read_bytes(), path
                    # Explicit empty override suppresses Keychain even after login.
                    run(["registry", "status"], env, root, fragment="empty; anonymous")
                    run(["registry", "logout"], stored_env, root, fragment="not been revoked")
                    run(["registry", "logout"], stored_env, root)
                    run(["registry", "status"], stored_env, root, fragment="anonymous")
                finally:
                    run(["registry", "logout"], stored_env, root)
        finally:
            server.shutdown()
            server.server_close()
            worker.join()
    print("Registry authentication model, transport and CLI checks passed.")


if __name__ == "__main__":
    main()
