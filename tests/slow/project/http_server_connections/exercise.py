"""Synchronize two peers with the package's single-thread driver on loopback."""
import contextlib
import pathlib
import select
import socket
import subprocess
import time


def line(server):
    if not select.select([server.stdout], [], [], 8)[0]:
        raise TimeoutError("server did not reach its next synchronization point")
    value = server.stdout.readline()
    if not value:
        raise RuntimeError(f"server exited during exchange: {server.poll()}")
    return value.strip()


def header(stream, expected):
    data = bytearray()
    while not data.endswith(b"\r\n\r\n"):
        byte = stream.recv(1)
        if not byte or len(data) > 4096:
            raise AssertionError(f"incomplete response: {data!r}")
        data.extend(byte)
    assert bytes(data) == expected, data


def exercise(exe, server):
    port = int(line(server))
    address = ("127.0.0.1", port)
    with socket.create_connection(address, timeout=5) as slow:
        slow.sendall(b"GET /bulk HTTP/1.1\r\nHo")
        assert line(server) == b"partial head"
        with socket.create_connection(address, timeout=5) as fast:
            fast.sendall(b"GET /monitors HTTP/1.1\r\nHost: local\r\n\r\n")
            assert line(server) == b"fast GET complete"
            header(fast, b"HTTP/1.1 200 \r\nContent-Length: 0\r\n\r\n")
            slow.sendall(b"st: local\r\n\r\n")
            assert line(server) == b"slow socket backpressured"
            # Queue two requests. The rejected one deliberately withholds its body.
            fast.sendall(b"POST /monitors HTTP/1.1\r\nHost: local\r\nContent-Length: 2\r\n\r\n{}"
                         b"POST /missing HTTP/1.1\r\nHost: local\r\nExpect: 100-continue\r\n"
                         b"Content-Length: 100\r\n\r\n")
            assert line(server) == b"fast POST complete"
            header(fast, b"HTTP/1.1 201 \r\nContent-Length: 0\r\n\r\n")
            header(slow, b"HTTP/1.1 200 \r\nContent-Length: 67108864\r\n\r\n")
            remaining = 64 * 1024 * 1024
            deadline = time.monotonic() + 10
            while remaining:
                if time.monotonic() > deadline:
                    raise TimeoutError("response failed to resume")
                data = slow.recv(min(65536, remaining))
                assert data and data.count(0) == len(data)
                remaining -= len(data)
            header(fast, b"HTTP/1.1 404 \r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            assert fast.recv(1) == b""
    subprocess.run([exe, f"http://127.0.0.1:{port}/monitors"], check=True, timeout=8)
    assert server.wait(timeout=5) == 0


def main():
    exe, = pathlib.Path("build/gti/dev").glob("*/dashboard")
    with subprocess.Popen([exe], stdout=subprocess.PIPE, bufsize=0) as server:
        try:
            exercise(exe, server)
        finally:
            if server.poll() is None:
                with contextlib.suppress(ProcessLookupError):
                    server.kill()
    print("resumed partial HTTP reads and writes")


if __name__ == "__main__":
    main()
