#!/usr/bin/env python3
"""Execute quick-reference examples and their loopback HTTP error paths."""

import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import threading
from http.server import ThreadingHTTPServer
from example_case_context import LLVM_ONLY_EXAMPLES, require_cpp_limitation


OUTPUTS = {
    "091-json-documents.gti": 'tea\n42\n{"title":"Improve examples","open":true,"labels":["documentation","good first issue"]}\ndocumentation\ngood first issue\n{\n  "title": "Improve examples",\n  "open": true,\n  "labels": [\n    "documentation",\n    "good first issue"\n  ]\n}\n',
    "092-json-codecs.gti": 'alice\n{"id":7,"login":"alice"}\none\ntwo\n/members/0/login\n',
    "093-http-json-client.gti": "https://example.com/api/users?name=Alice%20Smith\nUsage: http-json URL [post]\n",
    "094-binary-text-encoding.gti": "QQBC\n410042\n",
    "095-explicit-cloning.gti": "build\nbuild!\n",
    "096-error-mapping.gti": "8\nconfiguration error handled\n",
    "097-constexpr-tables.gti": "64\n768\n",
    "098-shared-ownership.gti": "player.png\nasset released\n",
}


def main():
    compiler = Path(sys.argv[1]).resolve()
    root = Path(__file__).resolve().parents[1]
    environment = dict(os.environ, GTI_STDLIB_PATH=str(root / "stdlib"),
                       NO_PROXY="127.0.0.1,localhost", no_proxy="127.0.0.1,localhost")
    handler = runpy.run_path(str(root / "examples/http-json-server.py"))["Handler"]
    with ThreadingHTTPServer(("127.0.0.1", 0), handler) as server:
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            with tempfile.TemporaryDirectory(prefix="gti-practical-examples-") as temp:
                for standard, level in (("c++20", "-O0"), ("c++23", "-O3")):
                    for name, output in OUTPUTS.items():
                        binary = Path(temp) / Path(name).stem
                        flags = ["--std", standard]
                        if name in LLVM_ONLY_EXAMPLES:
                            require_cpp_limitation(compiler, root / "examples" / name,
                                                   binary, environment, level[1:], standard)
                            flags = ["--backend", "llvm"]
                        built = subprocess.run(
                            [str(compiler), str(root / "examples" / name), *flags,
                             level, "-o", str(binary)], capture_output=True, text=True,
                            timeout=240, env=environment)
                        assert built.returncode == 0, (name, standard, built.stdout, built.stderr)
                        cases = [([], 0, output)]
                        if name == "093-http-json-client.gti":
                            base = f"http://127.0.0.1:{server.server_port}"
                            cases += [
                                ([base + "/user"], 0, "local demo\n"),
                                ([base + "/user", "post"], 0, "tea\n"),
                                ([base + "/not-found"], 6, "HTTP status rejected\n404\n"),
                                ([base + "/invalid-json"], 7, "Invalid JSON response\n"),
                                ([base + "/missing-name"], 8, "Missing or invalid name\n"),
                                ([base + "/oversize"], 5, "HTTP transport failed\n"),
                                (["file:///not-http"], 2, "Invalid HTTP(S) URL\n"),
                            ]
                        for args, status, expected in cases:
                            result = subprocess.run([str(binary), *args], capture_output=True,
                                                    text=True, timeout=15, env=environment)
                            assert (result.returncode, result.stdout, result.stderr) == (status, expected, ""), (
                                name, standard, args, result.returncode, result.stdout, result.stderr)
                        print(f"PASS {name} {standard} {level}", flush=True)
        finally:
            server.shutdown()
            worker.join(timeout=5)


if __name__ == "__main__":
    main()
