#!/usr/bin/env python3
"""Exercise <std/http> over local HTTP and certificate-verified HTTPS."""

from __future__ import annotations

import http.server
import gzip
import json
import os
import time
import pathlib
import ssl
import subprocess
import sys
import tempfile
import threading


class Handler(http.server.BaseHTTPRequestHandler):
    gate = threading.Event()
    cross_url = ""
    http_url = ""
    forbidden_hits = 0
    protocol_version = "HTTP/1.1"

    def handle(self) -> None:
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError):
            # Abandoning a streaming response deliberately closes its connection.
            pass

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

    def do_HEAD(self) -> None:
        self.send_bytes(200, b"ignored")

    def do_GET(self) -> None:
        if self.path == "/policy-forbidden":
            Handler.forbidden_hits += 1
            self.send_bytes(302, b"", (("Location", "/empty"),))
            return
        if self.path == "/policy-hop":
            self.send_bytes(302, b"", (("Location", "/policy-hop2"),))
            return
        if self.path == "/policy-hop2":
            self.send_bytes(302, b"", (("Location", "/empty"),))
            return
        if self.path == "/policy-downgrade":
            self.send_bytes(302, b"", (("Location", self.http_url + "policy-forbidden"),))
            return
        if self.path == "/trailers":
            self.wfile.write(b"HTTP/1.1 103 Early Hints\r\nLink: </early>\r\n\r\n")
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Trailer", "X-After")
            self.end_headers()
            self.wfile.write(b"1\r\nx\r\n0\r\nX-After: late\r\n\r\n")
            return
        if self.path == "/bad-chunk":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            self.wfile.write(b"not-hex\r\n")
            self.close_connection = True
            return
        if self.path == "/unsafe-scheme":
            self.send_bytes(302, b"", (("Location", "file:///tmp/not-http"),))
            return
        if self.path == "/unsafe-credentials":
            target = self.http_url.replace("http://", "http://user:secret@")
            self.send_bytes(302, b"", (("Location", target + "teapot"),))
            return
        if self.path == "/empty":
            self.send_bytes(204, b"")
            return
        if self.path == "/encoded":
            self.send_bytes(200, gzip.compress(b"encoded-body", mtime=0),
                            (("Content-Encoding", "gzip"),))
            return
        if self.path == "/gated":
            self.gate.clear()
            self.send_response(200)
            self.send_header("Content-Length", "7")
            self.send_header("X-Headers-Ready", "yes")
            self.end_headers()
            self.wfile.flush()
            if self.gate.wait(timeout=5):
                self.wfile.write(b"a\0b\0cde")
            self.close_connection = True
            return
        if self.path == "/release":
            self.gate.set()
            self.send_bytes(200, b"released")
            return
        if self.path == "/stream-large":
            self.send_response(200)
            self.send_header("Content-Length", str(2 * 1024 * 1024))
            self.end_headers()
            block = bytes(range(256)) * 256
            for _ in range(32):
                try:
                    self.wfile.write(block)
                except (BrokenPipeError, ConnectionResetError):
                    break
            return
        if self.path == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            self.wfile.write(b"4\r\n\x00\x01\xfe\xff\r\n4\r\n\x00\x01\xfe\xff\r\n0\r\n\r\n")
            return
        if self.path == "/truncated":
            self.send_response(200)
            self.send_header("Content-Length", "64")
            self.end_headers()
            self.wfile.write(b"abc")
            self.close_connection = True
            return
        if self.path == "/relative/start":
            self.send_bytes(302, b"", (("Location", "../teapot"),))
            return
        if self.path == "/loop":
            self.send_bytes(302, b"", (("Location", "/loop"),))
            return
        if self.path == "/cross":
            self.send_bytes(302, b"", (("Location", self.cross_url + "credentials"),))
            return
        if self.path == "/credentials":
            leaked = self.headers.get("Authorization") or self.headers.get("X-Secret")
            self.send_bytes(400 if leaked else 200, b"leaked" if leaked else b"clean")
            return
        if self.path == "/downgrade":
            self.send_bytes(302, b"", (("Location", self.http_url + "teapot"),))
            return
        if self.path == "/method":
            self.send_bytes(200, b"GET")
            return
        if self.path == "/slow":
            self.send_response(200)
            self.send_header("Content-Length", "4")
            self.end_headers()
            self.wfile.flush()
            time.sleep(0.5)
            try:
                self.wfile.write(b"slow")
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        if self.path == "/pause":
            time.sleep(0.25)
            self.send_bytes(200, b"")
            return
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
        if self.path in ("/post302", "/post307"):
            self.send_bytes(302 if self.path == "/post302" else 307, b"", (("Location", "/method"),))
            return
        if self.path == "/method":
            self.send_bytes(200, body)
            return
        marker = self.headers.get("X-GTI-Test", "")
        self.send_bytes(201, marker.encode() + b":" + body)


def run(command: list[str], timeout: int = 120,
        expected_code: int = 0) -> subprocess.CompletedProcess[str]:
    root = pathlib.Path(__file__).resolve().parent.parent
    environment = dict(os.environ, GTI_STDLIB_PATH=str(root / "stdlib"),
                       NO_PROXY="127.0.0.1,localhost", no_proxy="127.0.0.1,localhost")
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout,
                            env=environment)
    if result.returncode != expected_code:
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
    return f'''import std.http;
import std.string;
import std.vector;

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


def streaming_source_text(http_url: str, https_url: str, certificate: pathlib.Path) -> str:
    values = {
        "HTTP": json.dumps(http_url),
        "HTTPS": json.dumps(https_url),
        "HTTPS_UPPER": json.dumps(https_url.replace("https://", "HTTPS://")),
        "CA": json.dumps(str(certificate)),
        "ENCODED_SIZE": str(len(gzip.compress(b"encoded-body", mtime=0))),
        "ENCODED_SUM": str(sum(gzip.compress(b"encoded-body", mtime=0))),
    }
    source = r'''import std.http;
import std.io;
import std.span;
import std.cstdio;
import std.string;
import std.vector;

// A non-file writer checks generic I/O composition and handles short writes.
class counting_writer {
public:
  counting_writer() = default;
  mut uint64_t bytes = 0;
  mut uint64_t sum = 0;
  expected<std::size_t, std::io_errc> write_some(std::span<uint8_t>& input) mut {
    std::size_t count = input.size() > std::size_t(97) ? std::size_t(97) : input.size();
    for (mut std::size_t i = 0; i < count; i++) { this.sum += uint64_t(input[i]); }
    this.bytes += uint64_t(count);
    return count;
  }
  expected<void, std::io_errc> flush() mut { return; }
};

std::string endpoint(std::string_view suffix) {
  mut auto result = std::string(@HTTP@);
  result.append(suffix);
  return std::move(result);
}

expected<std::http::streaming_response, std::http::errc>
independent(std::string_view suffix) {
  auto made = ?std::http::client::create();
  mut auto client = std::move(made);
  auto address = endpoint(suffix);
  auto request = std::http::request(std::http::method::get, address.view());
  return client.send_stream(request);
}

expected<uint64_t, std::io_errc> drain(mut std::http::streaming_response& response) {
  mut std::byte_buffer bytes{std::size_t(113)};
  mut auto scratch = bytes.as_mutable_span();
  mut uint64_t count = 0;
  while (true) {
    auto read = ?response.read_some(scratch);
    if (read == 0) { return count; }
    count += uint64_t(read);
  }
}

int main() {
  auto made = std::http::client::create();
  if (!made) { return 1; }
  mut auto client = std::move(made).value();

  // Scheme restriction is independent of redirect policy and precedes I/O.
  for (mut int mode = 0; mode < 3; mode++) {
    auto address = endpoint("policy-forbidden");
    mut auto request = std::http::request(std::http::method::get, address.view());
    if (request.schemes_policy() != std::http::scheme_policy::http_and_https) { return 80; }
    request.set_scheme_policy(std::http::scheme_policy::https_only);
    if (request.schemes_policy() != std::http::scheme_policy::https_only) { return 81; }
    if (mode == 1) { request.set_redirect_policy(std::http::redirect_policy::same_origin); }
    if (mode == 2) { request.set_redirect_policy(std::http::redirect_policy::secure); }
    auto buffered = client.send(request);
    if (buffered || buffered.error() != std::http::errc::unsupported_scheme || client.last_native_code() != 0) { return 82; }
    auto streamed = client.send_stream(request);
    if (streamed || streamed.error() != std::http::errc::unsupported_scheme || client.last_native_code() != 0) { return 83; }

    // Uppercase HTTPS and multiple relative hops retain policy and CA trust.
    mut auto secure = std::string(@HTTPS_UPPER@);
    secure.append(mode == 0 ? "empty" : "policy-hop");
    request.set_url(secure.view());
    if (!request.set_ca_bundle(@CA@)) { return 84; }
    auto accepted = client.send(request);
    if (!accepted || accepted.value().status() != uint32_t(204)) { return 85; }
    auto accepted_stream = client.send_stream(request);
    if (!accepted_stream || accepted_stream.value().status() != uint32_t(204)) { return 86; }
    if (mode != 0) {
      mut auto down = std::string(@HTTPS_UPPER@);
      down.append("policy-downgrade");
      request.set_url(down.view());
      auto denied = client.send(request);
      if (denied || denied.error() != std::http::errc::redirect_rejected) { return 87; }
      auto denied_stream = client.send_stream(request);
      if (denied_stream || denied_stream.error() != std::http::errc::redirect_rejected) { return 88; }
    }
    request.set_url("HTTPS://");
    auto invalid = client.send(request);
    if (invalid || invalid.error() != std::http::errc::invalid_url) { return 89; }
    auto invalid_stream = client.send_stream(request);
    if (invalid_stream || invalid_stream.error() != std::http::errc::invalid_url) { return 90; }
    // Explicit opt-out permits the local HTTP server again.
    auto ordinary = endpoint("empty");
    request.set_url(ordinary.view());
    request.set_scheme_policy(std::http::scheme_policy::http_and_https);
    auto reset = client.send(request);
    if (!reset || reset.value().status() != uint32_t(204)) { return 91; }
  }

  // This endpoint cannot send a body until another request releases it.
  // send_stream must return headers without waiting for the whole body.
  auto gated_url = endpoint("gated");
  mut auto gated_request = std::http::request(std::http::method::get, gated_url.view());
  gated_request.set_timeout_ms(uint64_t(3000));
  auto first = client.send_stream(gated_request);
  if (!first) { return 2; }
  mut auto gated = std::move(first).value();
  if (gated.status() != uint32_t(200) || gated.bytes_read() != 0) { return 3; }
  auto headers = gated.headers();
  mut bool marker = false;
  for (auto& field : headers) {
    if (field.name() == "X-Headers-Ready" && field.value() == "yes") { marker = true; }
  }
  if (!marker) { return 4; }
  auto release_url = endpoint("release");
  auto released = client.get(release_url.view());
  if (!released) { return 5; }
  auto received = drain(gated);
  if (!received || received.value() != uint64_t(7) || gated.bytes_read() != uint64_t(7) || gated.last_error()) { return 6; }

  // Both request and client die in independent(), but response remains usable.
  auto surviving = independent("chunked");
  if (!surviving) { return 7; }
  mut auto chunked = std::move(surviving).value();
  mut counting_writer sink{};
  mut std::byte_buffer storage{std::size_t(4096)};
  mut auto scratch = storage.as_mutable_span();
  auto copied = std::io::copy(chunked, sink, scratch);
  if (!copied || copied.value() != std::size_t(8) || sink.bytes != uint64_t(8) || sink.sum != uint64_t(1020)) { return 8; }
  auto eof = chunked.read_some(scratch);
  if (!eof || eof.value() != 0) { return 9; }
  chunked.cancel(); // Completed EOF is not changed into a cancellation error.
  auto completed = chunked.read_some(scratch);
  if (!completed || completed.value() != 0 || chunked.last_error()) { return 63; }

  auto large_result = independent("stream-large");
  if (!large_result) { return 10; }
  mut auto large = std::move(large_result).value();
  mut counting_writer large_sink{};
  auto large_copy = std::io::copy(large, large_sink, scratch);
  if (!large_copy || large_sink.bytes != uint64_t(2097152) || large_sink.sum != uint64_t(267386880)) { return 11; }

  auto missing_result = independent("missing");
  if (!missing_result) { return 12; }
  mut auto missing = std::move(missing_result).value();
  if (missing.status() != uint32_t(404)) { return 13; }
  auto missing_size = drain(missing);
  if (!missing_size || missing_size.value() != uint64_t(7)) { return 14; }

  auto truncated_result = independent("truncated");
  if (!truncated_result) { return 15; }
  mut auto truncated = std::move(truncated_result).value();
  auto truncated_size = drain(truncated);
  if (truncated_size || !truncated.last_error()) { return 16; }
  auto truncated_again = truncated.read_some(scratch);
  if (truncated_again || truncated_again.error() != truncated_size.error()) { return 17; }

  auto limit_url = endpoint("chunked");
  mut auto limited_request = std::http::request(std::http::method::get, limit_url.view());
  limited_request.set_max_response_bytes(uint64_t(4));
  auto limited_result = client.send_stream(limited_request);
  if (!limited_result) { return 18; }
  mut auto limited = std::move(limited_result).value();
  auto limited_size = drain(limited);
  if (limited_size) { return 19; }
  auto limit_error = limited.last_error();
  if (!limit_error || limit_error.value() != std::http::errc::response_too_large) { return 20; }

  auto cancelled_result = independent("stream-large");
  if (!cancelled_result) { return 21; }
  mut auto cancelled = std::move(cancelled_result).value();
  cancelled.cancel();
  cancelled.cancel();
  auto cancelled_read = cancelled.read_some(scratch);
  auto cancelled_error = cancelled.last_error();
  if (cancelled_read || !cancelled_error || cancelled_error.value() != std::http::errc::cancelled) { return 22; }

  // Empty output does not advance a live response or imply end of stream.
  auto zero_result = independent("teapot");
  if (!zero_result) { return 23; }
  mut auto zero = std::move(zero_result).value();
  mut std::byte_buffer empty{};
  mut auto no_space = empty.as_mutable_span();
  auto zero_read = zero.read_some(no_space);
  if (!zero_read || zero_read.value() != 0 || zero.bytes_read() != 0) { return 24; }
  auto zero_size = drain(zero);
  if (!zero_size || zero_size.value() != uint64_t(6)) { return 25; }

  auto redir_url = endpoint("relative/start");
  mut auto redir_request = std::http::request(std::http::method::get, redir_url.view());
  redir_request.set_redirect_policy(std::http::redirect_policy::same_origin);
  redir_request.set_redirect_limit(uint32_t(3));
  auto redirected_result = client.send_stream(redir_request);
  if (!redirected_result) { return 26; }
  mut auto redirected = std::move(redirected_result).value();
  auto final_url = endpoint("teapot");
  if (redirected.status() != uint32_t(418) || redirected.effective_url() != final_url.view()) { return 27; }
  auto redirect_size = drain(redirected);
  if (!redirect_size || redirect_size.value() != uint64_t(6)) { return 28; }

  auto loop_url = endpoint("loop");
  mut auto loop_request = std::http::request(std::http::method::get, loop_url.view());
  loop_request.set_redirect_policy(std::http::redirect_policy::same_origin);
  loop_request.set_redirect_limit(uint32_t(2));
  auto loop = client.send_stream(loop_request);
  if (loop || loop.error() != std::http::errc::too_many_redirects) { return 29; }

  auto cross_url = endpoint("cross");
  mut auto cross_request = std::http::request(std::http::method::get, cross_url.view());
  cross_request.set_redirect_policy(std::http::redirect_policy::same_origin);
  auto cross_rejected = client.send_stream(cross_request);
  if (cross_rejected || cross_rejected.error() != std::http::errc::redirect_rejected) { return 30; }
  cross_request.set_redirect_policy(std::http::redirect_policy::secure);
  auto authorization = cross_request.set_header("Authorization", "secret");
  auto extra = cross_request.set_header("X-Secret", "also-secret");
  if (!authorization || !extra) { return 31; }
  auto cross_result = client.send_stream(cross_request);
  if (!cross_result) { return 32; }
  mut auto cross = std::move(cross_result).value();
  if (cross.status() != uint32_t(200)) { return 33; }
  auto cross_size = drain(cross);
  if (!cross_size || cross_size.value() != uint64_t(5)) { return 34; }

  mut auto secure_address = std::string(@HTTPS@);
  secure_address.append("downgrade");
  mut auto downgrade = std::http::request(std::http::method::get, secure_address.view());
  auto ca = downgrade.set_ca_bundle(@CA@);
  if (!ca) { return 35; }
  downgrade.set_redirect_policy(std::http::redirect_policy::secure);
  auto denied = client.send_stream(downgrade);
  if (denied || denied.error() != std::http::errc::redirect_rejected) { return 36; }

  auto post_url = endpoint("post302");
  mut auto post_request = std::http::request(std::http::method::post, post_url.view());
  post_request.set_body("post-body");
  post_request.set_redirect_policy(std::http::redirect_policy::same_origin);
  auto post302_result = client.send_stream(post_request);
  if (!post302_result) { return 37; }
  mut auto post302 = std::move(post302_result).value();
  if (post302.status() != uint32_t(200)) { return 38; }
  auto post302_size = drain(post302);
  if (!post302_size || post302_size.value() != uint64_t(3)) { return 39; }
  auto keep_url = endpoint("post307");
  post_request.set_url(keep_url.view());
  auto post307_result = client.send_stream(post_request);
  if (!post307_result) { return 40; }
  mut auto post307 = std::move(post307_result).value();
  auto post307_size = drain(post307);
  if (!post307_size || post307_size.value() != uint64_t(9)) { return 41; }

  auto slow_url = endpoint("slow");
  mut auto slow_request = std::http::request(std::http::method::get, slow_url.view());
  slow_request.set_timeout_ms(uint64_t(100));
  auto slow_result = client.send_stream(slow_request);
  if (!slow_result) { return 42; }
  mut auto slow = std::move(slow_result).value();
  auto pause_url = endpoint("pause");
  auto paused = client.get(pause_url.view());
  if (!paused) { return 43; }
  auto timed_read = slow.read_some(scratch);
  auto timed_error = slow.last_error();
  if (timed_read || timed_read.error() != std::io_errc::timed_out || !timed_error || timed_error.value() != std::http::errc::timeout) { return 44; }

  {
    auto abandoned_result = independent("stream-large");
    if (!abandoned_result) { return 45; }
    auto abandoned = std::move(abandoned_result).value();
  }
  auto reuse = independent("teapot");
  if (!reuse) { return 46; }
  mut auto reused = std::move(reuse).value();
  auto reused_size = drain(reused);
  if (!reused_size || reused_size.value() != uint64_t(6)) { return 47; }
  auto encoded_result = independent("encoded");
  if (!encoded_result) { return 48; }
  mut auto encoded = std::move(encoded_result).value();
  mut counting_writer encoded_sink{};
  auto encoded_copy = std::io::copy(encoded, encoded_sink, scratch);
  if (!encoded_copy || encoded_sink.bytes != uint64_t(@ENCODED_SIZE@) || encoded_sink.sum != uint64_t(@ENCODED_SUM@)) { return 49; }
  auto empty_result = independent("empty");
  if (!empty_result) { return 50; }
  mut auto no_body = std::move(empty_result).value();
  auto empty_size = drain(no_body);
  if (!empty_size || empty_size.value() != 0 || no_body.status() != uint32_t(204)) { return 51; }
  auto head_url = endpoint("teapot");
  auto head_request = std::http::request(std::http::method::head, head_url.view());
  auto head_result = client.send_stream(head_request);
  if (!head_result) { return 52; }
  mut auto head_response = std::move(head_result).value();
  auto head_size = drain(head_response);
  if (!head_size || head_size.value() != 0) { return 53; }
  auto buffered_redirect = client.send(redir_request);
  if (!buffered_redirect || buffered_redirect.value().status() != uint32_t(418)) { return 54; }
  auto default_redirect = independent("redirect");
  if (!default_redirect || default_redirect.value().status() != uint32_t(302)) { return 55; }
  auto trailers_result = independent("trailers");
  if (!trailers_result) { return 56; }
  mut auto trailers = std::move(trailers_result).value();
  auto trailers_size = drain(trailers);
  if (!trailers_size || trailers_size.value() != uint64_t(1) || trailers.status() != uint32_t(200)) { return 57; }
  auto trailer_headers = trailers.headers();
  for (auto& field : trailer_headers) {
    if (field.name() == "Link" || field.name() == "X-After") { return 58; }
  }
  auto malformed_result = independent("bad-chunk");
  if (!malformed_result) { return 59; }
  mut auto malformed = std::move(malformed_result).value();
  auto malformed_size = drain(malformed);
  if (malformed_size || !malformed.last_error()) { return 60; }
  auto unsafe_url = endpoint("unsafe-scheme");
  mut auto unsafe_request = std::http::request(std::http::method::get, unsafe_url.view());
  unsafe_request.set_redirect_policy(std::http::redirect_policy::secure);
  auto unsafe_scheme = client.send_stream(unsafe_request);
  if (unsafe_scheme || unsafe_scheme.error() != std::http::errc::redirect_rejected) { return 61; }
  auto credential_url = endpoint("unsafe-credentials");
  unsafe_request.set_url(credential_url.view());
  auto unsafe_credentials = client.send_stream(unsafe_request);
  if (unsafe_credentials || unsafe_credentials.error() != std::http::errc::redirect_rejected) { return 62; }
  auto live_address = endpoint("stream-large");
  auto live_request = std::http::request(std::http::method::get, live_address.view());
  auto live_first_result = client.send_stream(live_request);
  auto live_second_result = client.send_stream(live_request);
  if (!live_first_result || !live_second_result) { return 64; }
  mut auto live_first = std::move(live_first_result).value();
  mut auto live_second = std::move(live_second_result).value();
  live_first.cancel();
  auto second_size = drain(live_second);
  if (!second_size || second_size.value() != uint64_t(2097152)) { return 65; }
  auto after_cancel_result = client.send_stream(head_request);
  if (!after_cancel_result) { return 66; }
  mut auto after_cancel = std::move(after_cancel_result).value();
  auto after_cancel_size = drain(after_cancel);
  if (!after_cancel_size || after_cancel_size.value() != 0) { return 67; }
  auto encoded_url = endpoint("encoded");
  auto decoded_result = client.get(encoded_url.view());
  if (!decoded_result) { return 68; }
  auto decoded = std::move(decoded_result).value();
  auto decoded_body = decoded.body();
  if (decoded_body != "encoded-body") { return 69; }
  return 0;
}
'''
    for name, value in values.items():
        source = source.replace("@" + name + "@", value)
    return source


def main() -> None:
    compiler = pathlib.Path(sys.argv[1]).resolve()
    optimizations = tuple(sys.argv[2:]) or ("O0", "O3")
    root = pathlib.Path(__file__).resolve().parent.parent
    certificate = root / "tests/fixtures/http_test_cert.pem"
    key = root / "tests/fixtures/http_test_key.pem"
    http_server, http_thread = start_server()
    cross_server, cross_thread = start_server()
    https_server, https_thread = start_server(certificate, key)
    try:
        http_url = f"http://127.0.0.1:{http_server.server_port}/"
        https_url = f"https://localhost:{https_server.server_port}/"
        Handler.http_url = http_url
        Handler.cross_url = f"http://127.0.0.1:{cross_server.server_port}/"
        with tempfile.TemporaryDirectory(prefix="gti-http-") as directory:
            output = pathlib.Path(directory)
            source = output / "http_runtime.gti"
            source.write_text(
                source_text(http_url, https_url, certificate), encoding="utf-8"
            )
            streaming = output / "http_streaming.gti"
            streaming.write_text(streaming_source_text(http_url, https_url, certificate), encoding="utf-8")
            rejections = {
                "copy": ("import std.http;\nvoid duplicate(std::http::streaming_response& value) { auto duplicate = value; }\nint main() { return 0; }\n", "GTI-S2003"),
                "moved": ("import std.http;\nvoid invalid(std::http::streaming_response value) { auto moved = std::move(value); [[discard]] value.status(); }\nint main() { return 0; }\n", "GTI-S2018"),
                "forge": ("import std.http;\nint main() { auto forged = gti_internal::http_stream_handle(int64_t(1)); return 0; }\n", "GTI-S2058"),
            }
            for name, (text, diagnostic) in rejections.items():
                rejected = output / f"streaming-{name}.gti"
                rejected.write_text(text, encoding="utf-8")
                result = run([str(compiler), str(rejected), "--emit-mir", "-o",
                              str(output / f"streaming-{name}.mir")], expected_code=65)
                if diagnostic not in result.stderr:
                    raise AssertionError(f"missing {diagnostic}: {result.stderr}")
            for optimization in optimizations:
                binary = output / f"http-{optimization}"
                run(
                    [str(compiler), str(source), f"-{optimization}", "-o", str(binary)],
                    timeout=180,
                )
                result = run([str(binary)], timeout=60)
                if result.stdout or result.stderr:
                    raise AssertionError(f"unexpected HTTP fixture output: {result!r}")
                stream_binary = output / f"http-stream-{optimization}"
                run([str(compiler), str(streaming), f"-{optimization}", "-o", str(stream_binary)], timeout=180)
                streamed = run([str(stream_binary)], timeout=60)
                if Handler.forbidden_hits != 0:
                    raise AssertionError("HTTPS-only request contacted an HTTP endpoint")
                if streamed.stdout or streamed.stderr:
                    raise AssertionError(f"unexpected streaming fixture output: {streamed!r}")
    finally:
        http_server.shutdown()
        cross_server.shutdown()
        https_server.shutdown()
        http_server.server_close()
        cross_server.server_close()
        https_server.server_close()
        http_thread.join(timeout=10)
        cross_thread.join(timeout=10)
        https_thread.join(timeout=10)


if __name__ == "__main__":
    main()
