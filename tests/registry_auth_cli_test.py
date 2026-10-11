#!/usr/bin/env python3
"""Deterministic scoped-auth transport; OS Keychain roundtrip is opt-in."""
import hashlib
import base64
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
import re
import urllib.parse
from harness_timeouts import scaled

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
        if self.path == "/must-not-follow":
            self.server.followed_redirect = True
        authorization = self.headers.get("Authorization", "")
        record = self.server.tokens.get(authorization.removeprefix("token "))
        if authorization != "token " + SECRET and not record:
            self.reply(401, b"fixture-secret: server response must not be logged")
            return
        if record and record["username"] == "no-owner-access":
            self.reply(403)
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
        authorization = self.headers.get("Authorization", "")
        record = self.server.tokens.get(authorization.removeprefix("token "))
        if authorization != "token " + SECRET and (not record or record["scope"] != "write:package"):
            self.reply(403)
            return
        parts = urllib.parse.urlsplit(self.path).path.strip("/").split("/")
        key = tuple(parts[4:7])
        if key in self.server.files:
            self.reply(409)
            return
        self.server.files[key] = self.rfile.read(int(self.headers["Content-Length"]))
        self.reply(201)

    def basic(self):
        authorization = self.headers.get("Authorization", "")
        if not authorization.startswith("Basic "):
            self.reply(401)
            return None
        username, password = base64.b64decode(authorization[6:]).decode().split(":", 1)
        assert self.path.startswith(f"/api/v1/users/{urllib.parse.quote(username)}/tokens")
        if password not in ("fixture-password", "päss: wörd"):
            self.reply(401, b"fixture-password: private server response")
            return None
        if username == "otp-user" and self.headers.get("X-Gitea-OTP") != "123456":
            self.reply(401)
            return None
        return username

    def do_POST(self):
        self.server.requests += 1
        username = self.basic()
        if not username:
            return
        assert self.headers["Content-Type"] == "application/json"
        if username == "redirect-user":
            self.send_response(302)
            self.send_header("Location", "/must-not-follow")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        assert set(payload) == {"name", "scopes"}
        assert payload["scopes"] in (["read:package"], ["write:package"])
        name = payload["name"]
        assert re.fullmatch(r"gti-registry-[a-f0-9]{12}-[a-f0-9]{32}", name), name
        assert name not in self.server.issued_names
        self.server.issued_names.add(name)
        token = f"issued-{len(self.server.issued_names)}-fixture"
        self.server.tokens[token] = {"name": name, "username": username, "scope": payload["scopes"][0]}
        if username == "dropped-response-user":
            self.close_connection = True
            return
        if username == "error-response-user":
            self.reply(500)
            return
        if username in ("malformed-user", "cleanup-failure-user"):
            self.reply(201, b"fixture-password deliberately malformed secret response")
            return
        self.reply(201, json.dumps({"id": len(self.server.issued_names), "sha1": token,
                                   "name": "unrelated-token" if username == "forged-name-user" else name,
                                   "scopes": ["all"] if username == "bad-scope-user" else payload["scopes"]}).encode())

    def do_DELETE(self):
        self.server.requests += 1
        username = self.basic()
        if not username:
            return
        if username == "cleanup-failure-user":
            self.reply(403)
            return
        name = self.path.rsplit("/", 1)[1]
        assert name in self.server.issued_names, "must never revoke preexisting tokens"
        self.server.deleted_names.add(name)
        for token, record in list(self.server.tokens.items()):
            if record["name"] == name and record["username"] == username:
                del self.server.tokens[token]
                self.reply(204)
                return
        self.reply(404)


def run(args, env, cwd, text=None, success=True, fragment=None):
    result = subprocess.run([str(GTI), *args], env=env, cwd=cwd, input=text,
                            text=True, capture_output=True, timeout=scaled(30))
    output = result.stdout + result.stderr
    assert (result.returncode == 0) == success, (args, result.returncode, output)
    assert SECRET not in output, output
    for secret in ("fixture-password", "päss: wörd", "123456"):
        assert secret not in output, output
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
    process = subprocess.Popen([str(GTI), "registry", "login", "--token"], cwd=root,
                               env=env, stdin=slave, stdout=slave, stderr=slave)
    try:
        output = b""
        while b"input hidden" not in output:
            assert select.select([master], [], [], 10)[0], output
            output += os.read(master, 8192)
        assert not termios.tcgetattr(slave)[3] & termios.ECHO
        os.write(master, b"fixture-secret")
        process.send_signal(cancellation)
        assert process.wait(timeout=scaled(10)) != 0
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


def password_terminal(env, root, username="fixtureuser", password="fixture-password",
                      otp="", args=(), success=True, fragment=None, cancel=None):
    import pty
    import select
    import termios
    master, slave = pty.openpty()
    original = termios.tcgetattr(slave)
    process = subprocess.Popen([str(GTI), "registry", "login", *args], cwd=root,
                               env=env, stdin=slave, stdout=slave, stderr=slave)
    output = b""
    try:
        for label, value in [(b"Username: ", username), (b"Password (input hidden): ", password),
                             (b"2FA code (input hidden; leave empty if disabled): ", otp)]:
            while label not in output:
                assert select.select([master], [], [], 10)[0], output
                output += os.read(master, 8192)
            if label != b"Username: ":
                assert not termios.tcgetattr(slave)[3] & termios.ECHO
            if cancel and cancel[0] in label:
                os.write(master, value.encode())
                process.send_signal(cancel[1])
                break
            os.write(master, value.encode() + b"\n")
        status = process.wait(timeout=scaled(30))
        while select.select([master], [], [], 0)[0]:
            output += os.read(master, 8192)
        assert (status == 0) == success, (status, output)
        assert termios.tcgetattr(slave)[3] & termios.ECHO == original[3] & termios.ECHO
        for secret in (SECRET, password, otp, base64.b64encode(f"{username}:{password}".encode()).decode()):
            if secret:
                assert secret.encode() not in output, output
        if fragment:
            assert fragment.encode() in output, output
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
        server.tokens = {}
        server.issued_names = set()
        server.deleted_names = set()
        server.followed_redirect = False
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
                                    capture_output=True, text=True, timeout=scaled(30))
            assert result.returncode == 0, result.stdout + result.stderr
            assert SECRET not in result.stdout + result.stderr
            assert len(server.deleted_names) == 7, "rollback malformed, broad, forged, save/owner denial and uncertain issuance"
            assert sum(record["username"] == "cleanup-failure-user" for record in server.tokens.values()) == 1
            assert not server.followed_redirect, "never redirect account credentials"
            before = server.requests
            run(["registry", "status"], env, root, fragment="empty; anonymous")
            run(["registry", "status"], dict(env, GTI_TEST_REGISTRY_AUTH_TOKEN=SECRET),
                root, fragment="environment")
            assert server.requests == before, "status must not contact registry"
            for args in [["registry"], ["registry", "bad"],
                         ["registry", "logout", "--token-stdin"],
                         ["registry", "login", "--token", SECRET],
                         ["registry", "login", "--password", "fixture-password"],
                         ["registry", "login", "--token", "--token-stdin"],
                         ["registry", "login", "--publish", "--token-stdin"],
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
                    for field in (b"Password", b"2FA"):
                        password_terminal(env, root, otp="123456", success=False,
                                          cancel=(field, cancellation), fragment="GTI-B1725")
                password_terminal(env, root, password="wrong-password", success=False, fragment="GTI-B1713")
                password_terminal(env, root, username="otp-user", success=False, fragment="GTI-B1713")
                password_terminal(env, root, otp="12bad", success=False, fragment="GTI-B1725")
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
                    password_terminal(stored_env, root, fragment="macOS Keychain")
                    # Exercise real default-password CLI acquisition with no environment token.
                    second = root / "password-consumer"
                    second.mkdir()
                    (second / "gti.toml").write_text('manifest-version = 1\n[package]\nname = "second"\nversion = "0.1.0"\n')
                    run(["add", "secure"], stored_env, second)
                    publisher = root / "publisher"
                    publisher.mkdir()
                    (publisher / "gti.toml").write_text('manifest-version = 1\n[package]\nname = "password-package"\nversion = "1.0.0"\n')
                    (publisher / "src").mkdir()
                    (publisher / "src/unit.gti").write_text('namespace fixture { int value() { return 1; } }\n')
                    subprocess.run(["git", "init", "-q", str(publisher)], check=True, capture_output=True)
                    subprocess.run(["git", "-C", str(publisher), "add", "gti.toml", "src"], check=True, capture_output=True)
                    run(["publish", "--name", "cli-read-only"], stored_env, publisher, success=False, fragment="GTI-B1713")
                    password_terminal(stored_env, root, username="otp-user", otp="123456",
                                      args=("--publish",), fragment="macOS Keychain")
                    run(["publish", "--name", "cli-password-publish"], stored_env, publisher)
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
