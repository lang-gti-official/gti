#!/usr/bin/env python3
"""Exercise <std/http> over local HTTP and certificate-verified HTTPS."""

from __future__ import annotations

import http.server
import json
import pathlib
import ssl
import subprocess
import sys
import tempfile
import threading
from harness_timeouts import scaled


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format: str, *args: object) -> None:
        del format, args

    def send_bytes(
        self,
        status: int,
        body: bytes,
        headers: tuple[tuple[str, str], ...] = (),
    ) -> None:
        self.send_response(status)
        for name, value in headers:
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_GET(self) -> None:
        if self.path == "/url%20teapot?kind=tea%20pot":
            self.send_bytes(200, b"url-builder")
            return
        if self.path == "/teapot":
            self.send_bytes(
                418,
                b"teapot",
                (("X-Trace", "first"), ("X-Trace", "second")),
            )
            return
        if self.path == "/redirect":
            self.send_bytes(302, b"", (("Location", "/teapot"),))
            return
        if self.path == "/large":
            self.send_bytes(200, b"response-is-larger-than-four-bytes")
            return
        self.send_bytes(404, b"missing")

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        marker = self.headers.get("X-GTI-Test", "")
        self.send_bytes(201, marker.encode() + b":" + body)


def run(command: list[str], timeout: int = scaled(120)) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise AssertionError(
            f"{command!r}: exit {result.returncode}\n{result.stdout}{result.stderr}"
        )
    return result


def start_server(
    certificate: pathlib.Path | None = None, key: pathlib.Path | None = None
) -> tuple[http.server.ThreadingHTTPServer, threading.Thread]:
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    if certificate is not None and key is not None:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread


def source_text(http_url: str, https_url: str, certificate: pathlib.Path) -> str:
    http_base = json.dumps(http_url)
    http_teapot = json.dumps(http_url + "teapot")
    http_redirect = json.dumps(http_url + "redirect")
    http_large = json.dumps(http_url + "large")
    https_teapot = json.dumps(https_url + "teapot")
    https_base = json.dumps(https_url)
    certificate_literal = json.dumps(str(certificate))
    return f'''#include <std/http>
#include <std/string>
#include <std/vector>

int main() {{
  mut auto created = std::http::client::create();
  if (!created) {{
    return 1;
  }}
  mut auto client = std::move(created).value();

  mut std::http::request invalid_header = std::http::request(
      std::http::method::get, {http_teapot});
  mut auto invalid_header_result =
      invalid_header.add_header("X-Test", "bad\\r\\nvalue");
  if (invalid_header_result ||
      invalid_header_result.error() != std::http::errc::invalid_header) {{
    return 2;
  }}

  mut auto teapot_result = client.get({http_teapot});
  if (!teapot_result) {{
    return 3;
  }}
  mut auto teapot = std::move(teapot_result).value();
  if (teapot.status() != uint32_t(418)) {{
    return 4;
  }}
  mut std::string teapot_body = teapot.body();
  if (teapot_body != "teapot") {{
    return 5;
  }}
  mut std::vector<std::http::header> fields = teapot.headers();
  mut bool saw_first = false;
  mut bool saw_second = false;
  for (mut std::size_t index = std::size_t(0); index < fields.size(); index++) {{
    if (fields[index].name() == "X-Trace" &&
        fields[index].value() == "first") {{
      saw_first = true;
    }}
    if (fields[index].name() == "X-Trace" &&
        fields[index].value() == "second") {{
      saw_second = true;
    }}
  }}
  if (!saw_first || !saw_second) {{
    return 6;
  }}

  mut auto built_url_result = std::http::url::parse({http_base});
  if (!built_url_result) {{
    return 17;
  }}
  mut std::http::url built_url = std::move(built_url_result).value();
  built_url.push_path_segment("url teapot");
  built_url.add_query_parameter("kind", "tea pot");
  mut auto built_result = client.get(built_url);
  if (!built_result) {{
    return 18;
  }}
  mut auto built_response = std::move(built_result).value();
  if (built_response.status() != uint32_t(200)) {{
    return 19;
  }}
  mut std::string built_body = built_response.body();
  if (built_body != "url-builder") {{
    return 20;
  }}

  mut auto redirect_result = client.get({http_redirect});
  if (!redirect_result) {{
    return 7;
  }}
  mut auto redirect = std::move(redirect_result).value();
  if (redirect.status() != uint32_t(302)) {{
    return 8;
  }}

  mut std::http::request limited = std::http::request(
      std::http::method::get, {http_large});
  limited.set_max_response_bytes(uint64_t(4));
  mut auto limited_result = client.send(limited);
  if (limited_result ||
      limited_result.error() != std::http::errc::response_too_large) {{
    return 9;
  }}

  mut std::http::request unsupported = std::http::request(
      std::http::method::get, "file:///tmp/not-http");
  mut auto unsupported_result = client.send(unsupported);
  if (unsupported_result ||
      unsupported_result.error() != std::http::errc::unsupported_scheme) {{
    return 10;
  }}

  mut std::http::request untrusted = std::http::request(
      std::http::method::get, {https_teapot});
  untrusted.set_connect_timeout_ms(uint64_t(2000));
  untrusted.set_timeout_ms(uint64_t(3000));
  mut auto untrusted_result = client.send(untrusted);
  if (untrusted_result ||
      untrusted_result.error() != std::http::errc::tls_failed) {{
    return 11;
  }}

  mut auto trusted_url_result = std::http::url::parse({https_base});
  if (!trusted_url_result) {{
    return 21;
  }}
  mut std::http::url trusted_url = std::move(trusted_url_result).value();
  trusted_url.push_path_segment("echo");
  mut std::http::request trusted = std::http::request(
      std::http::method::post, trusted_url);
  mut auto ca_result = trusted.set_ca_bundle({certificate_literal});
  if (!ca_result) {{
    return 12;
  }}
  mut auto header_result = trusted.set_header("X-GTI-Test", "secure");
  if (!header_result) {{
    return 13;
  }}
  trusted.set_body("payload");
  mut auto trusted_result = client.send(trusted);
  if (!trusted_result) {{
    return 14;
  }}
  mut auto trusted_response = std::move(trusted_result).value();
  if (trusted_response.status() != uint32_t(201)) {{
    return 15;
  }}
  mut std::string trusted_body = trusted_response.body();
  if (trusted_body != "secure:payload") {{
    return 16;
  }}
  return 0;
}}
'''


def main() -> None:
    compiler = pathlib.Path(sys.argv[1]).resolve()
    optimizations = tuple(sys.argv[2:]) or ("O0", "O3")
    root = pathlib.Path(__file__).resolve().parent.parent
    certificate = root / "tests/fixtures/http_test_cert.pem"
    key = root / "tests/fixtures/http_test_key.pem"
    http_server, http_thread = start_server()
    https_server, https_thread = start_server(certificate, key)
    try:
        http_url = f"http://127.0.0.1:{http_server.server_port}/"
        https_url = f"https://localhost:{https_server.server_port}/"
        with tempfile.TemporaryDirectory(prefix="gti-http-") as directory:
            output = pathlib.Path(directory)
            source = output / "http_runtime.gti"
            source.write_text(
                source_text(http_url, https_url, certificate), encoding="utf-8"
            )
            for optimization in optimizations:
                binary = output / f"http-{optimization}"
                run(
                    [str(compiler), str(source), f"-{optimization}", "-o", str(binary)],
                    timeout=scaled(180),
                )
                result = run([str(binary)], timeout=scaled(60))
                if result.stdout or result.stderr:
                    raise AssertionError(f"unexpected HTTP fixture output: {result!r}")
    finally:
        http_server.shutdown()
        https_server.shutdown()
        http_server.server_close()
        https_server.server_close()
        http_thread.join(timeout=scaled(10))
        https_thread.join(timeout=scaled(10))


if __name__ == "__main__":
    main()
