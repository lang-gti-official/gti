#!/usr/bin/env python3
"""Offline deterministic Gitea adapter and complete package CLI workflow."""
import http.server
from contextlib import closing
from concurrent.futures import ThreadPoolExecutor
import json
import os
import pathlib
import shutil
import shlex
import subprocess
import sys
import tempfile
import threading
import urllib.parse

GTI = pathlib.Path(sys.argv[1]).resolve()
LSP = pathlib.Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class RegistryServer(http.server.ThreadingHTTPServer):
    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.files = {}
        self.requests = []
        self.fail_metadata = set()
        self.leaked_official_token = False
        self.require_read_token = False


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, body=b""):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.headers.get("Authorization") == "token official-only-fixture":
            self.server.leaked_official_token = True
        url = urllib.parse.urlsplit(self.path)
        parts = [urllib.parse.unquote(part) for part in url.path.split("/") if part]
        self.server.requests.append(("GET", tuple(parts)))
        if self.server.require_read_token and self.headers.get("Authorization") != "token fixture-secret":
            self.reply(401, b"fixture-secret server response must never enter diagnostics")
            return
        if parts[:5] == ["api", "v1", "packages", "test", "generic"]:
            package = parts[5]
            if len(parts) == 8 and parts[7] == "files":
                files = self.server.files.get((package, parts[6]))
                self.reply(200, json.dumps([{"name": name} for name in files]).encode()) if files else self.reply(404)
                return
            if len(parts) == 6:
                versions = sorted(version for name, version in self.server.files if name == package)
                query = urllib.parse.parse_qs(url.query)
                page, limit = int(query.get("page", [1])[0]), int(query.get("limit", [100])[0])
                self.reply(200, json.dumps([{"version": version} for version in versions[(page - 1) * limit:page * limit]]).encode())
                return
        if parts[:4] == ["api", "packages", "test", "generic"] and len(parts) == 7:
            body = self.server.files.get((parts[4], parts[5]), {}).get(parts[6])
            self.reply(200, body) if body is not None else self.reply(404)
            return
        self.reply(404)

    def do_PUT(self):
        parts = tuple(urllib.parse.unquote(part) for part in urllib.parse.urlsplit(self.path).path.split("/") if part)
        self.server.requests.append(("PUT", parts))
        body = self.rfile.read(int(self.headers["Content-Length"]))
        if self.headers.get("Authorization") != "token fixture-secret":
            self.reply(403, b"fixture-secret server response must never enter diagnostics")
            return
        if len(parts) != 7 or parts[:4] != ("api", "packages", "test", "generic"):
            self.reply(404)
            return
        name, version, file = parts[4:]
        if name in self.server.fail_metadata and file == "package.toml":
            self.reply(500)
            return
        files = self.server.files.setdefault((name, version), {})
        if file in files:
            self.reply(409)
            return
        files[file] = body
        self.reply(201)


def run(args, cwd, env, success=True, fragment=None):
    result = subprocess.run([str(GTI), *args], cwd=cwd, env=env, text=True, capture_output=True, timeout=120)
    output = result.stdout + result.stderr
    assert (result.returncode == 0) == success, (args, result.returncode, output)
    assert "Internal compiler error" not in output, output
    if fragment:
        assert fragment in output, output
    return output


def package(root, name, version, source, dependencies="", native=False):
    manifest = f'manifest-version = 1\n[package]\nname = "gti-{name}"\nversion = "{version}"\n'
    manifest += '[modules]\nunit = "src/unit.gti"\n'
    if native:
        manifest += '[package.native]\nc-standard = "c17"\nc-sources = ["native/account.c"]\n'
        write(root / "native/account.c", "#include <stdint.h>\nint64_t fixture_uid(void) { return 32; }\n")
    if dependencies:
        manifest += "[dependencies]\n" + dependencies
    write(root / "gti.toml", manifest)
    write(root / "src/unit.gti", "export module unit;\n" + source)
    subprocess.run(["git", "init", "-q", str(root)], check=True, capture_output=True)
    subprocess.run(["git", "-C", str(root), "add", "gti.toml", "src", *(["native"] if native else [])], check=True, capture_output=True)


def main():
    with tempfile.TemporaryDirectory(prefix="gti-registry-cli-") as directory:
        root = pathlib.Path(directory)
        server = RegistryServer()
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            config = root / "registries.toml"
            write(config, '[registries.official]\nbackend = "gitea-generic"\nowner = "test"\n'
                  f'url = "http://127.0.0.1:{server.server_port}"\ntoken-env = "GTI_TEST_REGISTRY_TOKEN"\n')
            env = dict(os.environ, GTI_REGISTRY_CONFIG=str(config), GTI_PACKAGE_CACHE=str(root / "cache"),
                       GTI_TEST_REGISTRY_TOKEN="fixture-secret", NO_COLOR="1")
            for version, number in [("1.0.0", 10), ("1.1.0", 20)]:
                helper = root / ("helper-" + version)
                package(helper, "helper", version, f"export namespace helper {{ int64_t value() {{ return {number}; }} }}\n")
                probe = helper / ".git/hooks/fsmonitor-probe"
                marker = root / "fsmonitor-must-not-run"
                write(probe, "#!/bin/sh\ntouch " + shlex.quote(str(marker)) + "\n")
                probe.chmod(0o755)
                subprocess.run(["git", "-C", str(helper), "config", "core.fsmonitor", str(probe)], check=True)
                run(["publish", "--name", "helper", "--dry-run"], helper, env, fragment="nothing uploaded")
                run(["publish", "--name", "helper"], helper, env)
                assert not marker.exists(), "publication ran a repository fsmonitor hook"
            libc = root / "libc"
            package(libc, "libc", "0.2.0", 'import helper.unit;\nnamespace libc { extern "C" { int64_t fixture_uid(); } export int64_t id() { return fixture_uid() + helper::value(); } }\n',
                    'helper = "^1.0"\n', native=True)
            run(["publish", "--name", "libc"], libc, env)
            run(["publish", "--name", "libc"], libc, env, False, "GTI-B1719")
            sequence = [parts[-1] for method, parts in server.requests if method == "PUT"]
            assert sequence == ["source.gtipkg", "package.toml"] * 3, sequence
            server.files[("libc", "0.3.0")] = {"source.gtipkg": b"incomplete"}
            # Publish authentication failures must not echo response bodies/tokens.
            unauthorized = root / "unauthorized"
            package(unauthorized, "unauthorized", "1.0.0", "int value() { return 1; }\n")
            bad_env = dict(env, GTI_TEST_REGISTRY_TOKEN="wrong-token")
            output = run(["publish"], unauthorized, bad_env, False, "GTI-B1713")
            assert "fixture-secret" not in output and "wrong-token" not in output, output
            assert "configured token-env" in output, output
            # Failure uploading final metadata leaves an unselectable version.
            broken = root / "broken"
            package(broken, "broken", "1.0.0", "int value() { return 1; }\n")
            server.fail_metadata.add("broken")
            run(["publish", "--name", "broken"], broken, env, False, "GTI-B1712")
            assert "package.toml" not in server.files[("broken", "1.0.0")]

            consumer = root / "consumer"
            original = '# Consumer comment must survive\nmanifest-version = 1\n[package]\nname = "consumer"\nversion = "0.1.0"\n\n[targets.app]\nkind = "executable"\nroot = "src/main.gti"\n'
            write(consumer / "gti.toml", original)
            source = 'import libc.unit;\nint main() { return libc::id() == int64_t(52) ? 0 : 1; }\n'
            write(consumer / "src/main.gti", source)
            consumer_env = dict(env, GTI_GIT=str(root / "must-not-run-git"), GTI_TEST_REGISTRY_TOKEN="")
            server.require_read_token = True
            output = run(["add", "libc"], consumer, consumer_env, False, "GTI-B1713")
            assert "configured token-env" in output and "fixture-secret" not in output, output
            assert (consumer / "gti.toml").read_text() == original and not (consumer / "gti.lock").exists()
            protected = root / "protected-consumer"
            write(protected / "gti.toml", original)
            write(protected / "src/main.gti", source)
            run(["add", "libc"], protected, env, fragment="Added libc ^0.2.0")
            server.require_read_token = False
            run(["add", "libc"], consumer, consumer_env, fragment="Added libc ^0.2.0")
            manifest = (consumer / "gti.toml").read_text()
            assert original in manifest and 'dependencies.libc = "^0.2.0"' in manifest, manifest
            locked = (consumer / "gti.lock").read_bytes()
            assert b'lock-version = 2' in locked and b'package = "helper"' in locked and b'official:helper@1.1.0' in locked
            assert b'127.0.0.1' not in locked and b'fixture-secret' not in locked
            run(["add", "libc"], consumer, consumer_env, fragment="already declared")
            run(["add", "libc@^0.3"], consumer, consumer_env, False, "GTI-B1723")
            assert (consumer / "gti.toml").read_text() == manifest
            for style in ("header", "inline"):
                project = root / style
                text = original + '\n[dependencies]\n# Keep the helper comment\nhelper = "^1.0"\n' if style == "header" else 'dependencies = {helper = "^1.0"}\n' + original
                write(project / "gti.toml", text)
                write(project / "src/main.gti", source)
                run(["add", "libc"], project, consumer_env)
                edited = (project / "gti.toml").read_text()
                assert '# Consumer comment must survive' in edited and 'helper = "^1.0"' in edited
                if style == "header":
                    assert '# Keep the helper comment' in edited
            # A newly added direct dependency may upgrade its old transitive
            # lock. Its distinct version is not an integrity violation.
            upgraded_helper = root / "helper-1.2.0"
            package(upgraded_helper, "helper", "1.2.0", "export namespace helper { int64_t value() { return 20; } }\n")
            run(["publish", "--name", "helper"], upgraded_helper, env)
            changed_requirement = root / "changed-requirement"
            # Use a valid dotted dependency independently of the
            # original manifest's current table context.
            write(changed_requirement / "gti.toml", 'dependencies.helper = "^1.2.0"\n' + manifest)
            write(changed_requirement / "src/main.gti", source)
            (changed_requirement / "gti.lock").write_bytes(locked)
            run(["fetch"], changed_requirement, consumer_env)
            assert b'official:helper@1.2.0' in (changed_requirement / "gti.lock").read_bytes()
            run(["add", "helper"], consumer, consumer_env, fragment="Added helper ^1.2.0")
            manifest = (consumer / "gti.toml").read_text()
            locked = (consumer / "gti.lock").read_bytes()
            assert b'official:helper@1.2.0' in locked
            # Metadata caching itself is checked even without a project lock.
            record = next(path for path in (root / "cache").rglob("metadata.record")
                          if b"registry-name = 'libc'" in path.read_bytes())
            good_record = record.read_bytes()
            record.write_bytes(good_record + b"\ncorrupt")
            run(["fetch"], consumer, consumer_env, False, "GTI-B1717")
            record.write_bytes(good_record)
            requests = len(server.requests)
            for std, optimization in [("c++20", "-O0"), ("c++23", "-O3")]:
                run(["run", "--locked", "--offline", "--no-cache", "--progress", "never", "--std", std, optimization], consumer, consumer_env)
            run(["metadata"], consumer, consumer_env, fragment="registry+official:libc@0.2.0")
            assert len(server.requests) == requests, "build/metadata must never query a registry"
            run(["fetch", "--offline"], consumer, consumer_env, fragment="up to date")
            assert (consumer / "gti.lock").read_bytes() == locked
            assert (consumer / "gti.toml").read_text() == manifest

            # The LSP must use the same cached package graph without requests.
            if LSP:
                from lsp_smoke_test import LspSession, lsp_position
                saved = dict(os.environ)
                os.environ.update(consumer_env)
                try:
                    with closing(LspSession(LSP)) as session:
                        session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}})
                        session.receive_until(lambda message: message.get("id") == 1)
                        session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
                        uri = (consumer / "src/main.gti").resolve().as_uri()
                        session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": source}}})
                        diagnostics = session.receive_until(lambda message: message.get("method") == "textDocument/publishDiagnostics" and message.get("params", {}).get("uri") == uri)["params"]["diagnostics"]
                        # The cached graph and ordinary module import are valid.
                        assert diagnostics == [], diagnostics
                        session.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover", "params": {"textDocument": {"uri": uri}, "position": lsp_position(source, source.index("id()") + 1)}})
                        hover = session.receive_until(lambda message: message.get("id") == 2)["result"]
                        assert hover and "int64_t" in str(hover), hover
                finally:
                    os.environ.clear()
                    os.environ.update(saved)
                assert len(server.requests) == requests, "LSP must not download registry packages"

            # Reproduce exact lock state after deleting the isolated cache.
            shutil.rmtree(root / "cache")
            before = len(server.requests)
            run(["fetch", "--locked"], consumer, consumer_env, fragment="up to date")
            assert (consumer / "gti.lock").read_bytes() == locked
            assert all(len(parts) != 6 for method, parts in server.requests[before:] if parts[:3] == ("api", "v1", "packages")), "locked fetch must not list versions"
            # Independent projects may populate the same immutable user cache.
            shutil.rmtree(root / "cache")
            projects = []
            for number in range(4):
                project = root / f"concurrent-{number}"
                write(project / "gti.toml", manifest)
                write(project / "src/main.gti", source)
                (project / "gti.lock").write_bytes(locked)
                projects.append(project)
            with ThreadPoolExecutor(max_workers=4) as pool:
                list(pool.map(lambda project: run(["fetch", "--locked"], project, consumer_env), projects))
            # Cached archives are not permission for metadata/build to unpack.
            tree = next((root / "cache").rglob("source/native/account.c")).parents[1]
            shutil.rmtree(tree)
            run(["metadata"], consumer, consumer_env, False, "GTI-B1703")
            assert not tree.exists()
            before = len(server.requests)
            run(["fetch", "--offline", "--locked"], consumer, consumer_env)
            assert tree.is_dir() and len(server.requests) == before
            # Reject tampered extracted sources and cached archive bytes.
            cached = next((root / "cache").rglob("source/native/account.c"))
            cached.write_text("corrupt")
            run(["build", "--progress", "never"], consumer, consumer_env, False, "GTI-B1717")
            shutil.rmtree(root / "cache")
            run(["fetch", "--locked"], consumer, consumer_env)
            cached = next((root / "cache").rglob("source.gtipkg"))
            cached.write_bytes(b"corrupt")
            run(["fetch"], consumer, consumer_env, False, "GTI-B1717")
            # Changed server bytes under an immutable version cannot relock.
            shutil.rmtree(root / "cache")
            good_archive = server.files[("libc", "0.2.0")]["source.gtipkg"]
            server.files[("libc", "0.2.0")]["source.gtipkg"] = b"mutated"
            run(["fetch"], consumer, consumer_env, False, "GTI-B1717")
            assert (consumer / "gti.lock").read_bytes() == locked
            server.files[("libc", "0.2.0")]["source.gtipkg"] = good_archive
            good_metadata = server.files[("libc", "0.2.0")]["package.toml"]
            server.files[("libc", "0.2.0")]["package.toml"] = good_metadata + b"\n# replacement\n"
            shutil.rmtree(root / "cache")
            run(["fetch"], consumer, consumer_env, False, "GTI-B1717")
            server.files[("libc", "0.2.0")]["package.toml"] = good_metadata
            assert (consumer / "gti.toml").read_text() == manifest

            # Resolver errors leave both project files unchanged.
            conflict = root / "conflict"
            write(conflict / "gti.toml", original + '\n[dependencies]\nhelper = "=2.0.0"\n')
            write(conflict / "src/main.gti", "int main() { return 0; }\n")
            unchanged = (conflict / "gti.toml").read_bytes()
            before = len(server.requests)
            run(["add", "libc"], conflict, consumer_env, False, "GTI-B1716")
            assert (conflict / "gti.toml").read_bytes() == unchanged and not (conflict / "gti.lock").exists()
            assert not any(parts[-1] == "source.gtipkg" for method, parts in server.requests[before:]), "conflicts must be detected before archive downloads"
            run(["fetch", "libc"], consumer, consumer_env, False, "unknown fetch option")
            run(["add", "missing"], consumer, consumer_env, False, "GTI-B1711")
            alternate = root / "alternate"
            write(alternate / "gti.toml", original)
            write(alternate / "src/main.gti", "int main() { return 0; }\n")
            alternate_config = root / "alternate.toml"
            write(alternate_config, '[registries.alternate]\nbackend = "gitea-generic"\nowner = "test"\n'
                  f'url = "http://127.0.0.1:{server.server_port}"\n')
            alternate_env = dict(consumer_env, GTI_REGISTRY_CONFIG=str(alternate_config), GTI_REGISTRY_TOKEN="official-only-fixture", GTI_REGISTRY_ALTERNATE_TOKEN="")
            run(["add", "helper", "--registry", "alternate"], alternate, alternate_env)
            assert not server.leaked_official_token, "a non-default registry inherited the official credential"
            write(alternate_config, '[registries.alternate]\nbackend = "unknown"\nurl = "https://invalid.example"\nowner = "test"\n')
            run(["add", "not_present", "--registry", "alternate"], alternate, alternate_env, False, "GTI-B1720")
            print("Registry model, Gitea publication, add/fetch/build/LSP/integrity workflow passed")
        finally:
            server.shutdown()
            server.server_close()
            worker.join()


if __name__ == "__main__":
    main()
